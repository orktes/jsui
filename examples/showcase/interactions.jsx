// Interaction showcase (1280 x 720): a game hub screen exercising what the
// static reference screens cannot show - hover / press / focus feedback
// driven by Animated springs, gamepad focus navigation, looping timing
// animations, a staggered entrance, a spring-y modal panel, a paging
// carousel (snapToInterval + Animated.event), a momentum FlatList with
// scroll indicators, a TextInput with caret / selection and LayoutAnimation
// on a chip list.
//
// It runs as written in React Native. Not part of the reference suite
// (animated and interactive); tools/jsui-record renders scripted sessions of
// it (examples/showcase/session.json). The app posts the page rectangles of
// a few elements as `jsui-record:anchors` so the session can aim at them.
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  Animated,
  AppRegistry,
  Easing,
  FlatList,
  Image,
  ImageBackground,
  LayoutAnimation,
  Modal,
  Pressable,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import { Host, useHostState } from '@jsui/react';
import { colors, font, rarity } from '../theme.js';

const NATIVE = true;  // useNativeDriver wherever React Native allows it (transform / opacity)

// --- anchors for tools/jsui-record ------------------------------------------
const anchorRects = {};
let anchorTimer = null;
function postAnchors() {
  if (anchorTimer) return;
  anchorTimer = setTimeout(() => {
    anchorTimer = null;
    Host.postMessage('jsui-record:anchors', anchorRects);
  }, 0);
}
function useAnchor(name) {
  const ref = useRef(null);
  const onLayout = useCallback(() => {
    const inst = ref.current;
    if (!inst || !inst.measureInWindow) return;
    inst.measureInWindow((x, y, width, height) => {
      anchorRects[name] = { x, y, width, height };
      postAnchors();
    });
  }, [name]);
  return { ref, onLayout };
}

const spring = (value, toValue, cfg) =>
  Animated.spring(value, Object.assign({ toValue, stiffness: 340, damping: 20, mass: 0.9, useNativeDriver: NATIVE }, cfg));

function useValue(v) {
  return useRef(new Animated.Value(v)).current;
}

// --- menu button: hover glow + scale, press squash, focus ring -------------
function MenuButton({ label, hint, badge, enter, anchor, onPress, danger }) {
  const hover = useValue(0);
  const press = useValue(0);
  const focus = useValue(0);
  const a = useAnchor(anchor);
  const translateX = Animated.add(enter.interpolate({ inputRange: [0, 1], outputRange: [-48, 0] }), hover.interpolate({ inputRange: [0, 1], outputRange: [0, 12] }));
  const scale = Animated.add(hover.interpolate({ inputRange: [0, 1], outputRange: [1, 1.035] }), press.interpolate({ inputRange: [0, 1], outputRange: [0, -0.06] }));
  return (
    <Pressable
      ref={a.ref}
      onLayout={a.onLayout}
      onHoverIn={() => spring(hover, 1).start()}
      onHoverOut={() => spring(hover, 0).start()}
      onPressIn={() => spring(press, 1, { stiffness: 600, damping: 26 }).start()}
      onPressOut={() => spring(press, 0, { stiffness: 260, damping: 12 }).start()}
      onFocus={(e) => e.nativeEvent.focusVisible && spring(focus, 1, { stiffness: 260, damping: 14 }).start()}
      onBlur={() => spring(focus, 0).start()}
      onPress={onPress}
      style={styles.menuHit}
    >
      <Animated.View style={[styles.menuBtn, { opacity: enter, transform: [{ translateX }, { scale }] }]}>
        <Animated.View style={[StyleSheet.absoluteFill, styles.menuGlow, { opacity: hover }]} />
        <Animated.View style={[styles.menuBar, { opacity: hover, transform: [{ scaleY: hover }] }]} />
        <View style={{ flex: 1 }}>
          <Text style={[styles.menuText, danger && { color: '#ff8fa3' }]}>{label}</Text>
          {hint ? <Text style={styles.menuHint}>{hint}</Text> : null}
        </View>
        {badge ? (
          <View style={styles.badge}>
            <Text style={styles.badgeText}>{badge}</Text>
          </View>
        ) : null}
        <Animated.Text style={[styles.chevron, { opacity: hover, transform: [{ translateX: hover.interpolate({ inputRange: [0, 1], outputRange: [-10, 0] }) }] }]}>›</Animated.Text>
      </Animated.View>
      <Animated.View
        pointerEvents="none"
        style={[styles.focusRing, { opacity: focus, transform: [{ translateX }, { scale: focus.interpolate({ inputRange: [0, 1], outputRange: [1.08, 1] }) }] }]}
      />
    </Pressable>
  );
}

// --- top tabs with a springing underline -----------------------------------
const TABS = ['Play', 'Armory', 'Codex', 'Social'];
const TAB_W = 104;
function Tabs() {
  const [tab, setTab] = useState(0);
  const x = useValue(0);
  const a = useAnchor('tabs');
  useEffect(() => {
    spring(x, tab * TAB_W, { stiffness: 260, damping: 18 }).start();
  }, [tab, x]);
  return (
    <View style={styles.tabs} ref={a.ref} onLayout={a.onLayout}>
      {TABS.map((t, i) => (
        <Pressable key={t} focusable={false} onPress={() => setTab(i)} style={styles.tab}>
          {({ hovered }) => <Text style={[styles.tabText, (hovered || tab === i) && styles.tabTextActive]}>{t}</Text>}
        </Pressable>
      ))}
      <Animated.View style={[styles.tabUnderline, { transform: [{ translateX: x }] }]} />
    </View>
  );
}

// --- season progress: timing fill (layout prop) + looping shimmer ----------
function SeasonCard() {
  const progress = useValue(0);
  const shimmer = useValue(0);
  const live = useValue(0);
  useEffect(() => {
    Animated.timing(progress, { toValue: 0.72, duration: 1800, delay: 500, easing: Easing.out(Easing.cubic), useNativeDriver: false }).start();
    Animated.loop(Animated.timing(shimmer, { toValue: 1, duration: 1700, easing: Easing.inOut(Easing.quad), useNativeDriver: NATIVE })).start();
    Animated.loop(
      Animated.sequence([
        Animated.timing(live, { toValue: 1, duration: 700, easing: Easing.inOut(Easing.sin), useNativeDriver: NATIVE }),
        Animated.timing(live, { toValue: 0, duration: 700, easing: Easing.inOut(Easing.sin), useNativeDriver: NATIVE }),
      ]),
    ).start();
  }, [progress, shimmer, live]);
  const [xp, setXp] = useState(0);
  useEffect(() => {
    const id = progress.addListener(({ value }) => setXp(Math.round(value * 25000)));
    return () => progress.removeListener(id);
  }, [progress]);
  return (
    <View style={styles.season}>
      <View style={styles.rowCenter}>
        <Animated.View style={[styles.liveDot, { opacity: live.interpolate({ inputRange: [0, 1], outputRange: [0.35, 1] }), transform: [{ scale: live.interpolate({ inputRange: [0, 1], outputRange: [0.8, 1.25] }) }] }]} />
        <Text style={styles.seasonKicker}>Season 4 · Emberfall</Text>
        <View style={{ flex: 1 }} />
        <Text style={styles.seasonTier}>Tier 37</Text>
      </View>
      <View style={styles.barTrack}>
        <Animated.View style={[styles.barFill, { width: progress.interpolate({ inputRange: [0, 1], outputRange: ['0%', '100%'] }) }]}>
          <Animated.View
            style={[styles.shimmer, { transform: [{ translateX: shimmer.interpolate({ inputRange: [0, 1], outputRange: [-80, 260] }) }, { skewX: '-20deg' }] }]}
          />
        </Animated.View>
      </View>
      <View style={styles.rowCenter}>
        <Text style={styles.xpText}>{String(xp).replace(/\B(?=(\d{3})+(?!\d))/g, ',')} / 25,000 XP</Text>
        <View style={{ flex: 1 }} />
        <Text style={styles.xpHint}>3 days left</Text>
      </View>
    </View>
  );
}

// --- featured carousel: paging with snapToInterval + parallax + dots -------
const CARD_W = 380;
const CARD_GAP = 16;
const SNAP = CARD_W + CARD_GAP;
const CARDS = [
  { art: require('../assets/card-ember.png'), kicker: 'Raid · Heroic', title: 'The Ember Gate', body: 'Storm the molten citadel before the tide turns.', tint: colors.orange },
  { art: require('../assets/card-tide.png'), kicker: 'Event · 2 days', title: 'Tidecaller Regatta', body: 'Race the drowned isles for Abyssal cosmetics.', tint: colors.cyan },
  { art: require('../assets/card-verdant.png'), kicker: 'Expedition', title: 'Verdant Reach', body: 'Chart the overgrown ruins with your squad.', tint: colors.green },
  { art: require('../assets/card-void.png'), kicker: 'Ranked · Season 4', title: 'Voidbreaker Trials', body: 'Climb the ladder. Top 500 earn the Void Crown.', tint: colors.purple },
];

function Carousel() {
  const scrollX = useValue(0);
  const a = useAnchor('carousel');
  return (
    <View>
      <View ref={a.ref} onLayout={a.onLayout} style={styles.carouselFrame}>
        <Animated.ScrollView
          horizontal
          snapToInterval={SNAP}
          decelerationRate="fast"
          disableIntervalMomentum
          showsHorizontalScrollIndicator={false}
          scrollEventThrottle={16}
          onScroll={Animated.event([{ nativeEvent: { contentOffset: { x: scrollX } } }], { useNativeDriver: NATIVE })}
          contentContainerStyle={{ paddingRight: 460 - CARD_W, gap: CARD_GAP }}
        >
          {CARDS.map((c, i) => {
            const input = [(i - 1) * SNAP, i * SNAP, (i + 1) * SNAP];
            const scale = scrollX.interpolate({ inputRange: input, outputRange: [0.9, 1, 0.9], extrapolate: 'clamp' });
            const parallax = scrollX.interpolate({ inputRange: input, outputRange: [-60, 0, 60], extrapolate: 'clamp' });
            const dim = scrollX.interpolate({ inputRange: input, outputRange: [0.55, 0, 0.55], extrapolate: 'clamp' });
            return (
              <Animated.View key={c.title} style={[styles.card, { transform: [{ scale }] }]}>
                <Animated.Image source={c.art} resizeMode="cover" style={[styles.cardArt, { transform: [{ translateX: parallax }, { scale: 1.3 }] }]} />
                <View style={styles.cardScrim} />
                <View style={styles.cardBody}>
                  <Text style={[styles.cardKicker, { color: c.tint }]}>{c.kicker}</Text>
                  <Text style={styles.cardTitle}>{c.title}</Text>
                  <Text style={styles.cardText}>{c.body}</Text>
                </View>
                <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, { backgroundColor: '#050710', opacity: dim }]} />
              </Animated.View>
            );
          })}
        </Animated.ScrollView>
      </View>
      <View style={styles.dots}>
        {CARDS.map((c, i) => {
          const input = [(i - 1) * SNAP, i * SNAP, (i + 1) * SNAP];
          return (
            <Animated.View
              key={c.title}
              style={[
                styles.dot,
                {
                  opacity: scrollX.interpolate({ inputRange: input, outputRange: [0.35, 1, 0.35], extrapolate: 'clamp' }),
                  transform: [{ scaleX: scrollX.interpolate({ inputRange: input, outputRange: [1, 3, 1], extrapolate: 'clamp' }) }],
                },
              ]}
            />
          );
        })}
      </View>
    </View>
  );
}

// --- squad: TextInput + LayoutAnimation chips ------------------------------
const SQUAD_ANIM = {
  duration: 420,
  create: { type: 'spring', property: 'scaleXY', springDamping: 0.55 },
  update: { type: 'spring', springDamping: 0.75 },
  delete: { type: 'easeOut', property: 'opacity', duration: 220 },
};
const CHIP_COLORS = [colors.gold, colors.cyan, colors.purple, colors.green, colors.orange, colors.red];

function Squad() {
  const [members, setMembers] = useState(['Valkyrie', 'Brann', 'Sorrel']);
  const [name, setName] = useState('');
  const [focused, setFocused] = useState(false);
  const ring = useValue(0);
  const input = useAnchor('recruit');
  const chips = useAnchor('chips');
  useEffect(() => {
    spring(ring, focused ? 1 : 0, { stiffness: 240, damping: 16 }).start();
  }, [focused, ring]);
  const add = () => {
    const n = name.trim();
    if (!n) return;
    LayoutAnimation.configureNext(SQUAD_ANIM);
    setMembers((m) => [n, ...m].slice(0, 4));
    setName('');
  };
  const remove = (n) => {
    LayoutAnimation.configureNext(SQUAD_ANIM);
    setMembers((m) => m.filter((x) => x !== n));
  };
  return (
    <View style={styles.squad}>
      <View style={styles.rowCenter}>
        <Text style={styles.sectionLabel}>Recruit squad</Text>
        <View style={{ flex: 1 }} />
        <Text style={styles.small}>{members.length} / 4</Text>
      </View>
      <View ref={input.ref} onLayout={input.onLayout} style={styles.inputWrap}>
        <Animated.View pointerEvents="none" style={[styles.inputGlow, { opacity: ring }]} />
        <TextInput
          value={name}
          onChangeText={setName}
          onSubmitEditing={add}
          onFocus={() => setFocused(true)}
          onBlur={() => setFocused(false)}
          placeholder="Type a callsign and press Enter…"
          placeholderTextColor={colors.textMute}
          selectionColor={colors.gold}
          cursorColor={colors.goldLight}
          maxLength={18}
          style={styles.input}
        />
      </View>
      <View ref={chips.ref} onLayout={chips.onLayout} style={styles.chips}>
        {members.map((m, i) => (
          <Chip key={m} name={m} color={CHIP_COLORS[(m.length + m.charCodeAt(0)) % CHIP_COLORS.length]} onRemove={() => remove(m)} />
        ))}
      </View>
    </View>
  );
}

function Chip({ name, color, onRemove }) {
  const a = useAnchor('chip-' + name);
  return (
    <View ref={a.ref} onLayout={a.onLayout} style={styles.chip}>
      <View style={[styles.chipAvatar, { backgroundColor: color }]}>
        <Text style={styles.chipInitial}>{name[0].toUpperCase()}</Text>
      </View>
      <Text style={styles.chipText}>{name}</Text>
      <Pressable focusable={false} onPress={onRemove} hitSlop={6} style={({ hovered, pressed }) => [styles.chipX, hovered && styles.chipXHover, pressed && { opacity: 0.6 }]}>
        <Text style={styles.chipXText}>×</Text>
      </Pressable>
    </View>
  );
}

// --- armory list: momentum FlatList with indicators -------------------------
const ICONS = {
  sword: require('../assets/item-sword.png'),
  shield: require('../assets/item-shield.png'),
  bow: require('../assets/item-bow.png'),
  helmet: require('../assets/item-helmet.png'),
  ring: require('../assets/item-ring.png'),
  boots: require('../assets/item-boots.png'),
  amulet: require('../assets/item-amulet.png'),
  axe: require('../assets/item-axe.png'),
  gem: require('../assets/item-gem.png'),
  scroll: require('../assets/item-scroll.png'),
  hp: require('../assets/item-potion-health.png'),
  mp: require('../assets/item-potion-mana.png'),
};
const NAMES = {
  sword: ['Dawnsplitter', 'Cinder Blade', 'Oathkeeper', 'Glasswing Saber'],
  shield: ['Bulwark of Ages', 'Tidewall', 'Emberguard'],
  bow: ['Whisperstring', 'Galecaller', 'Thornspire Longbow'],
  helmet: ['Crown of Ash', 'Warden Helm', 'Visage of Storms'],
  ring: ['Band of Echoes', 'Starlit Loop', 'Signet of Vows'],
  boots: ['Striders of Haste', 'Ashwalkers'],
  amulet: ['Heart of the Tide', 'Moonlit Locket'],
  axe: ['Worldcleaver', 'Frostbite Hatchet'],
  gem: ['Void Shard', 'Prism of Dusk'],
  scroll: ['Codex Fragment', 'Rite of Return'],
  hp: ['Phoenix Draught'],
  mp: ['Aether Tonic'],
};
const TIERS = ['legendary', 'epic', 'rare', 'uncommon', 'common'];
const ITEMS = (() => {
  const kinds = Object.keys(ICONS);
  const out = [];
  for (let i = 0; i < 48; i++) {
    const kind = kinds[(i * 5 + 3) % kinds.length];
    const names = NAMES[kind];
    const tier = TIERS[(i * 7 + (i >> 2)) % 5];
    out.push({ id: String(i), kind, name: names[(i >> 1) % names.length], tier, power: 980 - i * 13 - ((i * 37) % 23) });
  }
  return out;
})();

const ArmoryRow = React.memo(function ArmoryRow({ item }) {
  const r = rarity[item.tier];
  return (
    <Pressable style={({ hovered, pressed, focused }) => [styles.row, hovered && styles.rowHover, pressed && styles.rowPressed, focused && styles.rowFocused]}>
      <View style={[styles.rowIcon, { borderColor: r.color, boxShadow: `inset 0 0 10px ${r.glow}` }]}>
        <Image source={ICONS[item.kind]} style={styles.rowImg} />
      </View>
      <View style={{ flex: 1 }}>
        <Text style={styles.rowName} numberOfLines={1}>
          {item.name}
        </Text>
        <Text style={[styles.rowTier, { color: r.color }]}>{r.label}</Text>
      </View>
      <Text style={styles.rowPower}>{item.power}</Text>
    </Pressable>
  );
});

function Armory({ enter }) {
  const a = useAnchor('armory');
  return (
    <Animated.View style={[styles.armory, { opacity: enter, transform: [{ translateY: enter.interpolate({ inputRange: [0, 1], outputRange: [24, 0] }) }] }]}>
      <View style={styles.armoryHead}>
        <Text style={styles.sectionLabel}>Armory</Text>
        <View style={{ flex: 1 }} />
        <Text style={styles.small}>{ITEMS.length} items · Power</Text>
      </View>
      <View ref={a.ref} onLayout={a.onLayout} style={{ flex: 1 }}>
        <FlatList
          data={ITEMS}
          keyExtractor={(it) => it.id}
          renderItem={({ item }) => <ArmoryRow item={item} />}
          getItemLayout={(_, index) => ({ length: 60, offset: 60 * index, index })}
          indicatorStyle="white"
          initialNumToRender={14}
          windowSize={7}
          contentContainerStyle={{ paddingBottom: 8 }}
        />
      </View>
    </Animated.View>
  );
}

// --- deploy panel: spring-in modal with backdrop ----------------------------
function DeployPanel({ visible, onClose }) {
  const [mounted, setMounted] = useState(visible);
  const t = useValue(0);
  const a = useAnchor('panel');
  useEffect(() => {
    if (visible) {
      setMounted(true);
      spring(t, 1, { stiffness: 220, damping: 15, mass: 1 }).start();
    } else if (mounted) {
      spring(t, 0, { stiffness: 320, damping: 26, overshootClamping: true }).start(({ finished }) => finished && setMounted(false));
    }
  }, [visible]);  // eslint-disable-line react-hooks/exhaustive-deps
  if (!mounted) return null;
  return (
    <Modal transparent visible onRequestClose={onClose}>
      <Animated.View style={[StyleSheet.absoluteFill, styles.backdrop, { opacity: t.interpolate({ inputRange: [0, 1], outputRange: [0, 1], extrapolate: 'clamp' }) }]} />
      <View style={styles.panelCenter} pointerEvents="box-none">
        <Animated.View
          ref={a.ref}
          onLayout={a.onLayout}
          style={[
            styles.panel,
            {
              opacity: t.interpolate({ inputRange: [0, 0.6, 1], outputRange: [0, 1, 1], extrapolate: 'clamp' }),
              transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [60, 0] }) }, { scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.82, 1] }) }],
            },
          ]}
        >
          <Image source={require('../assets/card-ember.png')} style={styles.panelArt} resizeMode="cover" />
          <View style={styles.panelArtScrim} />
          <View style={styles.panelBody}>
            <Text style={[styles.cardKicker, { color: colors.orange }]}>Expedition ready</Text>
            <Text style={styles.panelTitle}>The Ember Gate</Text>
            <Text style={styles.panelText}>4 heroes · Heroic · Recommended power 900. Rewards: Cinderforged cache, 1,200 XP.</Text>
            <View style={styles.panelButtons}>
              <PanelButton label="Deploy squad" primary anchor="panel-confirm" onPress={onClose} />
              <PanelButton label="Cancel" anchor="panel-cancel" onPress={onClose} />
            </View>
          </View>
        </Animated.View>
      </View>
    </Modal>
  );
}

function PanelButton({ label, primary, anchor, onPress }) {
  const hover = useValue(0);
  const focus = useValue(0);
  const a = useAnchor(anchor);
  return (
    <Pressable
      ref={a.ref}
      onLayout={a.onLayout}
      autoFocus={primary}
      onPress={onPress}
      onHoverIn={() => spring(hover, 1).start()}
      onHoverOut={() => spring(hover, 0).start()}
      onFocus={(e) => e.nativeEvent.focusVisible && spring(focus, 1).start()}
      onBlur={() => spring(focus, 0).start()}
    >
      {({ pressed }) => (
        <Animated.View style={[styles.pbtn, primary && styles.pbtnPrimary, { transform: [{ scale: pressed ? 0.95 : hover.interpolate({ inputRange: [0, 1], outputRange: [1, 1.06] }) }] }]}>
          <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, styles.pbtnGlow, { opacity: hover }]} />
          <Animated.View pointerEvents="none" style={[styles.pbtnRing, { opacity: focus }]} />
          <Text style={[styles.pbtnText, primary && { color: '#2a1a03' }]}>{label}</Text>
        </Animated.View>
      )}
    </Pressable>
  );
}

function DeployButton({ onPress }) {
  const hover = useValue(0);
  const pulse = useValue(0);
  const a = useAnchor('deploy');
  useEffect(() => {
    Animated.loop(Animated.timing(pulse, { toValue: 1, duration: 1800, easing: Easing.out(Easing.quad), useNativeDriver: NATIVE })).start();
  }, [pulse]);
  return (
    <Pressable
      ref={a.ref}
      onLayout={a.onLayout}
      onPress={onPress}
      onHoverIn={() => spring(hover, 1).start()}
      onHoverOut={() => spring(hover, 0).start()}
      style={styles.deployHit}
    >
      {({ pressed }) => (
        <Animated.View style={[styles.deploy, { transform: [{ scale: pressed ? 0.96 : hover.interpolate({ inputRange: [0, 1], outputRange: [1, 1.04] }) }] }]}>
          <Animated.View
            pointerEvents="none"
            style={[styles.deployPulse, { opacity: pulse.interpolate({ inputRange: [0, 1], outputRange: [0.7, 0] }), transform: [{ scaleX: pulse.interpolate({ inputRange: [0, 1], outputRange: [1, 1.08] }) }, { scaleY: pulse.interpolate({ inputRange: [0, 1], outputRange: [1, 1.45] }) }] }]}
          />
          <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, styles.deployShine, { opacity: hover }]} />
          <Text style={styles.deployText}>Deploy expedition</Text>
          <Text style={styles.deployHint}>The Ember Gate · Heroic</Text>
        </Animated.View>
      )}
    </Pressable>
  );
}

// --- screen ------------------------------------------------------------------
const MENU = [
  { label: 'Continue', hint: 'Act III · The Ember Gate', anchor: 'menu-0' },
  { label: 'New Journey', anchor: 'menu-1' },
  { label: 'Multiplayer', badge: '3 invites', anchor: 'menu-2' },
  { label: 'Armory', anchor: 'menu-3' },
  { label: 'Settings', anchor: 'menu-4' },
  { label: 'Quit to Desktop', danger: true, anchor: 'menu-5' },
];

function KeyHint({ glyph, label, color }) {
  return (
    <View style={styles.keyHint}>
      <View style={[styles.keyGlyph, { borderColor: color }]}>
        <Text style={[styles.keyGlyphText, { color }]}>{glyph}</Text>
      </View>
      <Text style={styles.keyLabel}>{label}</Text>
    </View>
  );
}

// The player chip reads shared host state: a game sets it with
// jsui_set_state(rt, "player", "{\"credits\":12480,...}") and patches it as
// the numbers change (a jsui-record timeline with {"state": "player", ...}).
// Without host state it shows the defaults.
const PLAYER = { name: 'Valkyrie_07', level: 42, role: 'Vanguard', credits: 12480 };
const thousands = (n) => String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ',');

function Profile() {
  const player = useHostState('player', PLAYER);
  return (
    <View style={styles.profile}>
      <View style={styles.currency}>
        <View style={styles.coin} />
        <Text style={styles.currencyText}>{thousands(player.credits ?? PLAYER.credits)}</Text>
      </View>
      <View style={styles.divider} />
      <Image source={require('../assets/crest-wolf.png')} style={styles.crest} />
      <View>
        <Text style={styles.profileName}>{player.name ?? PLAYER.name}</Text>
        <Text style={styles.profileLevel}>{`Level ${player.level ?? PLAYER.level} · ${player.role ?? PLAYER.role}`}</Text>
      </View>
    </View>
  );
}

function App() {
  const enters = useMemo(() => MENU.map(() => new Animated.Value(0)), []);
  const panelsIn = useValue(0);
  const glow = useValue(0);
  const [deploy, setDeploy] = useState(false);
  useEffect(() => {
    Animated.parallel([
      Animated.stagger(80, enters.map((v) => spring(v, 1, { stiffness: 170, damping: 16 }))),
      Animated.timing(panelsIn, { toValue: 1, duration: 700, delay: 250, easing: Easing.out(Easing.cubic), useNativeDriver: NATIVE }),
    ]).start();
    Animated.loop(
      Animated.sequence([
        Animated.timing(glow, { toValue: 1, duration: 4000, easing: Easing.inOut(Easing.sin), useNativeDriver: NATIVE }),
        Animated.timing(glow, { toValue: 0, duration: 4000, easing: Easing.inOut(Easing.sin), useNativeDriver: NATIVE }),
      ]),
    ).start();
  }, [enters, panelsIn, glow]);

  return (
    <ImageBackground source={require('../assets/bg-menu.jpg')} style={styles.root} resizeMode="cover">
      <View style={styles.scrim} />
      <Animated.View
        pointerEvents="none"
        style={[styles.aurora, { opacity: glow.interpolate({ inputRange: [0, 1], outputRange: [0.35, 0.8] }), transform: [{ translateX: glow.interpolate({ inputRange: [0, 1], outputRange: [-60, 60] }) }] }]}
      />

      <View style={styles.topBar}>
        <Text style={styles.logo}>AETHERFALL</Text>
        <Tabs />
        <View style={{ flex: 1 }} />
        <Profile />
      </View>

      <View style={styles.left}>
        <Text style={styles.sectionLabel}>Main menu</Text>
        <View style={styles.menu}>
          {MENU.map((m, i) => (
            <MenuButton key={m.label} {...m} enter={enters[i]} onPress={m.label === 'Continue' ? () => setDeploy(true) : undefined} />
          ))}
        </View>
        <Animated.View style={{ opacity: panelsIn }}>
          <SeasonCard />
        </Animated.View>
      </View>

      <Animated.View style={[styles.center, { opacity: panelsIn, transform: [{ translateY: panelsIn.interpolate({ inputRange: [0, 1], outputRange: [24, 0] }) }] }]}>
        <View style={styles.rowCenter}>
          <Text style={styles.sectionLabel}>Featured</Text>
          <View style={{ flex: 1 }} />
          <Text style={styles.small}>Swipe · LB / RB</Text>
        </View>
        <Carousel />
        <Squad />
        <DeployButton onPress={() => setDeploy(true)} />
      </Animated.View>

      <Armory enter={panelsIn} />

      <View style={styles.bottomBar} pointerEvents="none">
        <KeyHint glyph="A" label="Select" color="#5fe08a" />
        <KeyHint glyph="B" label="Back" color="#ff6b81" />
        <KeyHint glyph="Y" label="Recruit" color="#ffd36b" />
        <Text style={styles.version}>v4.2.0 · 60 fps</Text>
      </View>

      <DeployPanel visible={deploy} onClose={() => setDeploy(false)} />
    </ImageBackground>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: colors.bg0 },
  scrim: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage:
      'linear-gradient(90deg, rgba(5,7,16,0.93) 0%, rgba(5,7,16,0.78) 40%, rgba(5,7,16,0.55) 75%, rgba(5,7,16,0.8) 100%), linear-gradient(180deg, rgba(5,7,16,0.5) 0%, rgba(5,7,16,0) 22%, rgba(5,7,16,0) 70%, rgba(5,7,16,0.9) 100%)',
  },
  aurora: {
    position: 'absolute',
    left: 260,
    top: -160,
    width: 760,
    height: 420,
    borderRadius: 380,
    experimental_backgroundImage: 'radial-gradient(circle at 50% 50%, rgba(242,195,91,0.22) 0%, rgba(79,140,255,0.08) 45%, rgba(5,7,16,0) 70%)',
  },
  rowCenter: { flexDirection: 'row', alignItems: 'center' },
  sectionLabel: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 2.2, textTransform: 'uppercase', color: colors.gold },
  small: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute },

  topBar: { position: 'absolute', left: 40, right: 32, top: 18, height: 48, flexDirection: 'row', alignItems: 'center' },
  logo: {
    fontFamily: font,
    fontWeight: '700',
    fontSize: 22,
    letterSpacing: 5,
    color: '#fff6dc',
    textShadowColor: 'rgba(242,195,91,0.6)',
    textShadowOffset: { width: 0, height: 0 },
    textShadowRadius: 14,
    marginRight: 40,
  },
  tabs: { flexDirection: 'row', height: 48 },
  tab: { width: TAB_W, height: 48, alignItems: 'center', justifyContent: 'center' },
  tabText: { fontFamily: font, fontWeight: '600', fontSize: 15, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.textMute },
  tabTextActive: { color: colors.text },
  tabUnderline: {
    position: 'absolute',
    left: 24,
    bottom: 4,
    width: TAB_W - 48,
    height: 3,
    borderRadius: 2,
    backgroundColor: colors.gold,
    boxShadow: '0 0 10px rgba(242,195,91,0.9)',
  },
  profile: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    paddingVertical: 6,
    paddingLeft: 16,
    paddingRight: 8,
    borderRadius: 26,
    backgroundColor: 'rgba(10, 14, 28, 0.72)',
    borderWidth: 1,
    borderColor: colors.line,
  },
  currency: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  coin: {
    width: 14,
    height: 14,
    borderRadius: 7,
    experimental_backgroundImage: 'radial-gradient(circle at 35% 35%, #fff2c2 0%, #f2c35b 45%, #a8741f 100%)',
    boxShadow: '0 0 8px rgba(242,195,91,0.5)',
  },
  currencyText: { fontFamily: font, fontWeight: '700', fontSize: 14, color: colors.text },
  divider: { width: 1, height: 24, backgroundColor: colors.line },
  crest: { width: 34, height: 34, borderRadius: 17, borderWidth: 2, borderColor: colors.gold },
  profileName: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text, paddingRight: 8 },
  profileLevel: { fontFamily: font, fontWeight: '500', fontSize: 11, color: colors.textDim },

  left: { position: 'absolute', left: 40, top: 96, width: 316 },
  menu: { marginTop: 12, gap: 6 },
  menuHit: { height: 50 },
  menuBtn: {
    height: 50,
    flexDirection: 'row',
    alignItems: 'center',
    paddingHorizontal: 18,
    borderRadius: 10,
    backgroundColor: 'rgba(14, 20, 38, 0.6)',
    borderWidth: 1,
    borderColor: 'rgba(148, 170, 255, 0.12)',
    overflow: 'hidden',
  },
  menuGlow: {
    borderRadius: 10,
    borderWidth: 1,
    borderColor: 'rgba(242, 195, 91, 0.9)',
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(242,195,91,0.34) 0%, rgba(242,195,91,0.04) 100%)',
    boxShadow: 'inset 0 0 18px rgba(242,195,91,0.22)',
  },
  menuBar: { position: 'absolute', left: 0, top: 10, bottom: 10, width: 4, borderTopRightRadius: 3, borderBottomRightRadius: 3, backgroundColor: colors.goldLight },
  menuText: { fontFamily: font, fontWeight: '600', fontSize: 15, letterSpacing: 1.4, textTransform: 'uppercase', color: '#d6ddf2' },
  menuHint: { fontFamily: font, fontWeight: '500', fontSize: 11, color: colors.gold, marginTop: 2, letterSpacing: 0.4 },
  chevron: { fontFamily: font, fontSize: 28, color: colors.goldLight, marginTop: -4 },
  focusRing: {
    position: 'absolute',
    left: -5,
    top: -5,
    right: -5,
    bottom: -5,
    borderRadius: 14,
    borderWidth: 2,
    borderColor: colors.cyan,
    boxShadow: '0 0 16px rgba(79,209,255,0.75)',
  },
  badge: { paddingHorizontal: 8, paddingVertical: 3, borderRadius: 10, backgroundColor: colors.red, boxShadow: '0 0 10px rgba(255,77,106,0.6)', marginRight: 6 },
  badgeText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: '#fff' },

  season: {
    marginTop: 18,
    padding: 14,
    gap: 10,
    borderRadius: 12,
    backgroundColor: 'rgba(12, 17, 32, 0.82)',
    borderWidth: 1,
    borderColor: colors.line,
  },
  liveDot: { width: 8, height: 8, borderRadius: 4, backgroundColor: colors.red, marginRight: 8, boxShadow: '0 0 8px rgba(255,77,106,0.9)' },
  seasonKicker: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.textDim },
  seasonTier: { fontFamily: font, fontWeight: '700', fontSize: 14, color: colors.goldLight },
  barTrack: { height: 10, borderRadius: 5, backgroundColor: 'rgba(148,170,255,0.12)', overflow: 'hidden' },
  barFill: {
    height: 10,
    borderRadius: 5,
    overflow: 'hidden',
    experimental_backgroundImage: 'linear-gradient(90deg, #a8741f 0%, #f2c35b 70%, #ffe29a 100%)',
    boxShadow: '0 0 12px rgba(242,195,91,0.6)',
  },
  shimmer: { position: 'absolute', top: -4, bottom: -4, width: 40, experimental_backgroundImage: 'linear-gradient(90deg, rgba(255,255,255,0) 0%, rgba(255,255,255,0.75) 50%, rgba(255,255,255,0) 100%)' },
  xpText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.text },
  xpHint: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute },

  center: { position: 'absolute', left: 392, top: 96, width: 460 },
  carouselFrame: { marginTop: 12, width: 460, height: 236 },
  card: { width: CARD_W, height: 236, borderRadius: 14, overflow: 'hidden', backgroundColor: colors.panelSolid, borderWidth: 1, borderColor: colors.lineStrong },
  cardArt: { position: 'absolute', left: 0, top: 0, width: CARD_W, height: 236 },
  cardScrim: { ...StyleSheet.absoluteFillObject, experimental_backgroundImage: 'linear-gradient(180deg, rgba(5,7,16,0) 30%, rgba(5,7,16,0.92) 100%)' },
  cardBody: { position: 'absolute', left: 18, right: 18, bottom: 16 },
  cardKicker: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.6, textTransform: 'uppercase' },
  cardTitle: { fontFamily: font, fontWeight: '700', fontSize: 24, color: colors.text, marginTop: 4 },
  cardText: { fontFamily: font, fontSize: 13, color: colors.textDim, marginTop: 4 },
  dots: { flexDirection: 'row', gap: 16, marginTop: 12, paddingLeft: 10, height: 6, alignItems: 'center' },
  dot: { width: 6, height: 6, borderRadius: 3, backgroundColor: colors.gold },

  squad: { marginTop: 16, padding: 14, borderRadius: 12, backgroundColor: 'rgba(12, 17, 32, 0.82)', borderWidth: 1, borderColor: colors.line, gap: 10 },
  inputWrap: { height: 40, borderRadius: 9, backgroundColor: 'rgba(5,8,18,0.85)', borderWidth: 1, borderColor: colors.lineStrong, justifyContent: 'center' },
  inputGlow: { position: 'absolute', left: -1, top: -1, right: -1, bottom: -1, borderRadius: 9, borderWidth: 1.5, borderColor: colors.gold, boxShadow: '0 0 14px rgba(242,195,91,0.45)' },
  input: { fontFamily: font, fontSize: 15, color: colors.text, paddingHorizontal: 12, height: 38 },
  chips: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, minHeight: 32 },
  chip: {
    flexDirection: 'row',
    alignItems: 'center',
    height: 32,
    paddingLeft: 4,
    paddingRight: 4,
    borderRadius: 16,
    backgroundColor: 'rgba(26, 36, 64, 0.95)',
    borderWidth: 1,
    borderColor: colors.lineStrong,
  },
  chipAvatar: { width: 24, height: 24, borderRadius: 12, alignItems: 'center', justifyContent: 'center' },
  chipInitial: { fontFamily: font, fontWeight: '700', fontSize: 12, color: '#0b0f1c' },
  chipText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text, marginLeft: 8, marginRight: 4 },
  chipX: { width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center' },
  chipXHover: { backgroundColor: 'rgba(255,77,106,0.35)' },
  chipXText: { fontFamily: font, fontWeight: '600', fontSize: 15, color: colors.textDim, marginTop: -2 },

  deployHit: { marginTop: 14 },
  deploy: {
    height: 58,
    borderRadius: 12,
    alignItems: 'center',
    justifyContent: 'center',
    experimental_backgroundImage: 'linear-gradient(180deg, #ffe29a 0%, #f2c35b 45%, #c08a2a 100%)',
    boxShadow: '0 10px 30px rgba(242,195,91,0.35), inset 0 1px 0 rgba(255,255,255,0.6)',
  },
  deployPulse: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, borderRadius: 12, borderWidth: 2, borderColor: colors.goldLight },
  deployShine: { borderRadius: 12, experimental_backgroundImage: 'linear-gradient(180deg, rgba(255,255,255,0.45) 0%, rgba(255,255,255,0) 60%)' },
  deployText: { fontFamily: font, fontWeight: '700', fontSize: 17, letterSpacing: 2, textTransform: 'uppercase', color: '#2a1a03' },
  deployHint: { fontFamily: font, fontWeight: '600', fontSize: 11, color: 'rgba(42,26,3,0.75)', marginTop: 1 },

  armory: {
    position: 'absolute',
    right: 32,
    top: 96,
    width: 352,
    bottom: 76,
    borderRadius: 14,
    backgroundColor: 'rgba(10, 14, 28, 0.86)',
    borderWidth: 1,
    borderColor: colors.line,
    boxShadow: '0 18px 40px rgba(0,0,0,0.5)',
    overflow: 'hidden',
  },
  armoryHead: { flexDirection: 'row', alignItems: 'center', paddingHorizontal: 16, height: 44, borderBottomWidth: 1, borderBottomColor: colors.line },
  row: { height: 60, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 14, gap: 12, borderBottomWidth: 1, borderBottomColor: 'rgba(148,170,255,0.06)' },
  rowHover: { backgroundColor: 'rgba(79,140,255,0.14)' },
  rowPressed: { backgroundColor: 'rgba(242,195,91,0.22)' },
  rowFocused: { backgroundColor: 'rgba(79,209,255,0.16)' },
  rowIcon: { width: 42, height: 42, borderRadius: 8, borderWidth: 1.5, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(5,8,18,0.8)' },
  rowImg: { width: 34, height: 34 },
  rowName: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
  rowTier: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1, textTransform: 'uppercase', marginTop: 2 },
  rowPower: { fontFamily: font, fontWeight: '700', fontSize: 16, color: colors.goldLight },

  backdrop: { backgroundColor: 'rgba(3,5,12,0.72)' },
  panelCenter: { ...StyleSheet.absoluteFillObject, alignItems: 'center', justifyContent: 'center' },
  panel: {
    width: 520,
    borderRadius: 18,
    overflow: 'hidden',
    backgroundColor: colors.panelSolid,
    borderWidth: 1,
    borderColor: 'rgba(242,195,91,0.45)',
    boxShadow: '0 30px 80px rgba(0,0,0,0.7), 0 0 40px rgba(242,195,91,0.18)',
  },
  panelArt: { width: 520, height: 170 },
  panelArtScrim: { position: 'absolute', left: 0, right: 0, top: 0, height: 170, experimental_backgroundImage: 'linear-gradient(180deg, rgba(18,26,46,0) 40%, rgba(18,26,46,1) 100%)' },
  panelBody: { padding: 22, paddingTop: 6 },
  panelTitle: { fontFamily: font, fontWeight: '700', fontSize: 28, color: colors.text, marginTop: 4 },
  panelText: { fontFamily: font, fontSize: 14, lineHeight: 21, color: colors.textDim, marginTop: 8 },
  panelButtons: { flexDirection: 'row', gap: 12, marginTop: 20 },
  pbtn: {
    height: 46,
    paddingHorizontal: 26,
    borderRadius: 10,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 'rgba(26,36,64,0.95)',
    borderWidth: 1,
    borderColor: colors.lineStrong,
  },
  pbtnPrimary: { experimental_backgroundImage: 'linear-gradient(180deg, #ffe29a 0%, #f2c35b 50%, #c08a2a 100%)', borderColor: 'rgba(255,226,154,0.9)' },
  pbtnGlow: { borderRadius: 10, backgroundColor: 'rgba(255,255,255,0.14)' },
  pbtnRing: { position: 'absolute', left: -5, top: -5, right: -5, bottom: -5, borderRadius: 14, borderWidth: 2, borderColor: colors.cyan, boxShadow: '0 0 14px rgba(79,209,255,0.75)' },
  pbtnText: { fontFamily: font, fontWeight: '700', fontSize: 14, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.text },

  bottomBar: {
    position: 'absolute',
    left: 0,
    right: 0,
    bottom: 0,
    height: 52,
    paddingRight: 32,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'flex-end',
    gap: 26,
    borderTopWidth: 1,
    borderTopColor: 'rgba(148,170,255,0.10)',
    backgroundColor: 'rgba(5,7,16,0.6)',
  },
  keyHint: { flexDirection: 'row', alignItems: 'center', gap: 9 },
  keyGlyph: { width: 22, height: 22, borderRadius: 11, borderWidth: 2, alignItems: 'center', justifyContent: 'center' },
  keyGlyphText: { fontFamily: font, fontWeight: '700', fontSize: 11 },
  keyLabel: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textDim },
  version: { fontFamily: font, fontSize: 12, color: colors.textMute, letterSpacing: 0.5, marginLeft: 12 },
});

AppRegistry.registerComponent('Showcase', () => App);
export default App;
