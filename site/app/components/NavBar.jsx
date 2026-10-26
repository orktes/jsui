// Fixed top bar: wordmark, page links, the sound switch and GitHub. When the
// links do not fit (measured, not a breakpoint) a menu button takes their
// place and opens them as a sheet under the bar (MobileMenu).
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Overlay, Pressable, ScrollView, StyleSheet, Text, View, Visibility, useHostState } from 'react-native';
import { colors, font, mono, site } from '../theme.js';
import { segments, useRouter } from '../router.js';
import { setSoundMuted, useAudioState } from '../sound.js';
import { Button, GITHUB, useLayout } from './ui.jsx';

export const NAV_HEIGHT = 64;

export const LINKS = [
  { to: '/screens', label: 'Screens' },
  { to: '/interactions', label: 'Interactions' },
  { to: '/input', label: 'Input' },
  { to: '/motion', label: 'Motion' },
  { to: '/vector', label: 'Vector' },
  { to: '/playground', label: 'Playground' },
  { to: '/embed', label: 'Embed' },
];

export function Logo({ size = 22 }) {
  const box = Math.round(size * 1.35);
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
      <View style={[styles.mark, { width: box, height: box, borderRadius: box * 0.28 }]}>
        <View style={[styles.markInner, { width: box * 0.42, height: box * 0.42, borderRadius: box * 0.1 }]} />
      </View>
      <Text style={[styles.word, { fontSize: size }]}>JSUI</Text>
    </View>
  );
}

function NavLink({ to, label, active }) {
  const { go } = useRouter();
  return (
    <Pressable onPress={() => go(to)} style={{ cursor: 'pointer' }}>
      {({ hovered, focused }) => (
        <View style={[styles.link, (hovered || focused) && styles.linkHot, active && styles.linkActive]}>
          <Text style={[styles.linkText, (hovered || focused || active) && { color: colors.text }]}>{label}</Text>
          {active ? <View style={styles.underline} /> : null}
        </View>
      )}
    </Pressable>
  );
}

// Sound on / off (the page mutes its Web Audio output and remembers it).
// Until the first click or key press the browser keeps audio locked: the
// switch says so ("Enable sound"), and pressing it is that gesture.
const EQ = [0.45, 0.85, 0.6, 1];
function SoundSwitch({ compact }) {
  const { unlocked, muted } = useAudioState();
  const on = unlocked && !muted;
  const label = muted ? 'Sound off' : unlocked ? 'Sound on' : 'Enable sound';
  return (
    <Pressable
      onPress={() => setSoundMuted(unlocked ? !muted : false)}
      accessibilityRole="switch"
      accessibilityLabel={label}
      accessibilityState={{ checked: on }}
      style={{ cursor: 'pointer', alignSelf: 'center', marginRight: compact ? 0 : 8 }}
    >
      {({ hovered, focused }) => (
        <View style={[styles.sound, (hovered || focused) && styles.linkHot]}>
          <View style={styles.eq}>
            {EQ.map((h, i) => (
              <View key={i} style={[styles.eqBar, { height: on ? 4 + h * 12 : 3, backgroundColor: on ? colors.gold : colors.textDim }]} />
            ))}
            {muted ? <View style={styles.eqSlash} /> : null}
          </View>
          {compact ? null : (
            <Text style={[styles.linkText, { fontSize: 13 }, (hovered || focused) && { color: colors.text }, !unlocked && !muted && { color: colors.gold }]}>
              {label}
            </Text>
          )}
        </View>
      )}
    </Pressable>
  );
}

// The menu button: three bars that fold into an X as the menu's progress `p`
// goes 0 -> 1 (native; the open spring's overshoot gives the X a settle).
function MenuButton({ p, open, onPress }) {
  const fx = useMemo(() => {
    const at = (out) => p.interpolate({ inputRange: [0, 1], outputRange: out });
    return {
      top: { transform: [{ translateY: at([0, 6]) }, { rotate: at(['0deg', '45deg']) }] },
      mid: {
        opacity: p.interpolate({ inputRange: [0, 0.5], outputRange: [1, 0], extrapolate: 'clamp' }),
        transform: [{ scaleX: p.interpolate({ inputRange: [0, 0.5], outputRange: [1, 0.2], extrapolate: 'clamp' }) }],
      },
      bottom: { transform: [{ translateY: at([0, -6]) }, { rotate: at(['0deg', '-45deg']) }] },
    };
  }, [p]);
  return (
    <Pressable
      onPress={onPress}
      accessibilityRole="button"
      accessibilityLabel={open ? 'Close menu' : 'Open menu'}
      accessibilityState={{ expanded: open }}
      style={styles.burger}
      hoveredStyle={styles.burgerHot}
      focusVisibleStyle={styles.burgerFocus}
      pressedStyle={{ transform: [{ scale: 0.92 }] }}
      transition={{ duration: 140, pressed: 50 }}
    >
      <Animated.View style={[styles.burgerBar, fx.top]} />
      <Animated.View style={[styles.burgerBar, fx.mid]} />
      <Animated.View style={[styles.burgerBar, fx.bottom]} />
    </Pressable>
  );
}

// One row of the menu sheet. It slides and fades in on its own slice of `p`,
// so the rows cascade in on open and fold away bottom-first on close.
function MenuItem({ p, index, to, label, active, autoFocus, onGo, style }) {
  const fx = useMemo(() => {
    const a = 0.1 + index * 0.06;
    const range = { inputRange: [a, a + 0.42], extrapolate: 'clamp' };
    return {
      opacity: p.interpolate({ ...range, outputRange: [0, 1] }),
      transform: [{ translateX: p.interpolate({ ...range, outputRange: [-22, 0] }) }],
    };
  }, [p, index]);
  return (
    <Animated.View style={[fx, style]}>
      <Pressable
        onPress={() => onGo(to)}
        autoFocus={autoFocus}
        accessibilityRole="link"
        accessibilityState={{ selected: active }}
        group
        style={[styles.item, active && styles.itemActive]}
        hoveredStyle={styles.itemHot}
        focusVisibleStyle={styles.itemFocus}
        pressedStyle={{ transform: [{ scale: 0.98 }] }}
        transition={{ duration: 120, pressed: 40 }}
      >
        {active ? <View style={styles.itemBar} /> : null}
        <Text style={[styles.itemNum, active && { color: colors.gold }]}>{String(index + 1).padStart(2, '0')}</Text>
        <Text style={[styles.itemText, active && { color: colors.text }]} groupHoveredStyle={{ color: colors.text }} transition={120}>
          {label}
        </Text>
        <Text style={[styles.itemChevron, active && { color: colors.gold }]} groupHoveredStyle={{ color: colors.text, transform: [{ translateX: 3 }] }} transition={140}>
          ›
        </Text>
      </Pressable>
    </Animated.View>
  );
}

// The sheet under the bar (a card under the menu button when there is room
// beside it) over a dimmed backdrop that closes it when pressed. It is an
// <Overlay> of the bar, so it draws above the page and its layout, and a <Visibility> on `p` takes it out of paint, hit testing and
// Tab order on the frame the close animation reaches 0. Everything moves on
// `p` alone, natively. A sheet taller than the room (a phone on its side)
// lays the rows out in two columns when the window is wide enough, never
// grows past the window, and scrolls whatever still doesn't fit.
function MobileMenu({ p, open, opens, head, width, height, gutter, phone, onGo, onClose }) {
  const fx = useMemo(
    () => ({
      backdrop: { opacity: p.interpolate({ inputRange: [0, 0.6], outputRange: [0, 1], extrapolate: 'clamp' }) },
      sheet: {
        opacity: p.interpolate({ inputRange: [0, 0.35], outputRange: [0, 1], extrapolate: 'clamp' }),
        transform: [
          { translateY: p.interpolate({ inputRange: [0, 1], outputRange: [-18, 0] }) },
          { scaleY: p.interpolate({ inputRange: [0, 1], outputRange: [0.94, 1] }) },
        ],
      },
      footer: { opacity: p.interpolate({ inputRange: [0.45, 0.9], outputRange: [0, 1], extrapolate: 'clamp' }) },
    }),
    [p],
  );
  // Focus moves into the sheet on every open (the rows mount again, keyed by
  // `opens`, so autoFocus fires once it shows): Escape, B and Tab then work
  // whether it was opened by touch, mouse, keyboard or gamepad.
  const focusAt = Math.max(0, LINKS.findIndex((l) => l.to === head));
  const room = height - (phone ? 0 : CARD_TOP + gutter / 2);
  const cols = !phone && LINKS.length * ITEM_HEIGHT + SHEET_CHROME > room ? 2 : 1;
  const card = { width: Math.min(cols === 2 ? 560 : 380, width - gutter * 2), marginRight: gutter };
  return (
    <Overlay placement="bottom" matchWidth flip={false} collision="none" hideWhenClipped={false}>
      <Visibility visible={p} style={{ height }}>
        <Animated.View style={[StyleSheet.absoluteFill, fx.backdrop]}>
          <Pressable onPress={onClose} focusable={false} accessibilityLabel="Close menu" style={[StyleSheet.absoluteFill, styles.backdrop]} />
        </Animated.View>
        <Animated.View
          accessibilityViewIsModal={open}
          onKeyDown={(e) => {
            if (e.key === 'Escape') {
              onClose();
              e.preventDefault();
              e.stopPropagation();
            }
          }}
          onGamepadButton={(e) => {
            if (e.buttonIndex === 1) {
              if (e.pressed) onClose();
              e.preventDefault();
              e.stopPropagation();
            }
          }}
          style={[styles.sheet, { maxHeight: room }, phone ? { paddingHorizontal: gutter - 6 } : [styles.card, card], fx.sheet]}
        >
          <View style={[styles.sheetGlow, phone && { borderTopLeftRadius: 0, borderTopRightRadius: 0 }]} pointerEvents="none" />
          <ScrollView style={styles.sheetRows} contentContainerStyle={cols === 2 && styles.grid}>
            {LINKS.map((l, i) => (
              <MenuItem key={l.to + opens} p={p} index={i} {...l} active={head === l.to} autoFocus={i === focusAt} onGo={onGo} style={cols === 2 && styles.gridCell} />
            ))}
          </ScrollView>
          <Animated.View style={[styles.sheetFooter, fx.footer]}>
            <Text style={styles.footerNote}>Rendered by JSUI, on the GPU, in this canvas.</Text>
            <Button small kind="ghost" label="GitHub" icon="★" href={GITHUB} self="center" />
          </Animated.View>
        </Animated.View>
      </Visibility>
    </Overlay>
  );
}

export function NavBar() {
  const { path, go } = useRouter();
  const { width, height, tablet, phone, gutter } = useLayout();
  const head = '/' + (segments(path)[0] || '');
  // A native window whose buttons sit in this bar (jsui-viewer
  // --titlebar-overlay): the wordmark starts after them.
  const controls = useHostState('jsui.windowControls', null);
  const left = controls ? Math.max(gutter, controls.x + controls.width + 28) : gutter;

  // Do the links fit? Both layouts stay laid out (the hidden one keeps its
  // size, see Visibility mode="hide"), so the full layout's link row and
  // trailing buttons can be measured against the room after the wordmark
  // whichever one shows: no breakpoint, no flip-flop. GUESS (about where the
  // row stops fitting) only covers the first frames, before the measures.
  const [room, setRoom] = useState(0);
  const [row, setRow] = useState(0);
  const [trail, setTrail] = useState(0);
  const measured = room > 0 && row > 0 && trail > 0;
  const compact = measured ? row + TRAIL_GAP + trail > room : width < GUESS;

  const p = useRef(new Animated.Value(0)).current;
  const [open, setOpen] = useState(false);
  const [opens, setOpens] = useState(0);
  const toggle = (on) => {
    setOpen(on);
    if (on) setOpens((n) => n + 1);
    p.stopAnimation();
    if (on) Animated.spring(p, { toValue: 1, stiffness: 340, damping: 26, mass: 1, useNativeDriver: true }).start();
    else Animated.timing(p, { toValue: 0, duration: 220, easing: Easing.in(Easing.cubic), useNativeDriver: true }).start();
  };
  // Leaving compact mode (a wider window) closes the menu.
  useEffect(() => {
    if (!compact && open) toggle(false);
  }, [compact]);
  const goFromMenu = (to) => {
    toggle(false);
    if (to !== head) go(to);
  };

  return (
    <View style={[styles.bar, { paddingHorizontal: gutter, paddingLeft: left }]}>
      <Pressable onPress={() => go('/')} style={{ cursor: 'pointer' }} focusable={false}>
        <Logo size={phone ? 18 : 20} />
      </Pressable>
      <View style={[styles.main, { marginLeft: tablet ? 12 : 36 }]} onLayout={(e) => setRoom(e.nativeEvent.layout.width)}>
        <Visibility visible={!compact} mode="hide" pointerEvents="box-none" style={[styles.ruler, { left: 0, alignItems: 'flex-start' }]}>
          <View style={styles.cluster} onLayout={(e) => setRow(e.nativeEvent.layout.width)}>
            {LINKS.map((l) => (
              <NavLink key={l.to} {...l} active={head === l.to} />
            ))}
          </View>
        </Visibility>
        <Visibility visible={!compact} mode="hide" pointerEvents="box-none" style={[styles.ruler, { right: 0, alignItems: 'flex-end' }]}>
          <View style={styles.cluster} onLayout={(e) => setTrail(e.nativeEvent.layout.width)}>
            <SoundSwitch compact={tablet} />
            {phone ? null : <Button small kind="ghost" label="GitHub" icon="★" href={GITHUB} self="center" />}
          </View>
        </Visibility>
        <Visibility visible={compact} mode="hide" style={[styles.cluster, styles.pinned, { gap: BURGER_GAP }]}>
          <SoundSwitch compact />
          <MenuButton p={p} open={open} onPress={() => toggle(!open)} />
        </Visibility>
      </View>
      {compact ? (
        <MobileMenu p={p} open={open} opens={opens} head={head} width={width} height={height - NAV_HEIGHT} gutter={gutter} phone={phone} onGo={goFromMenu} onClose={() => toggle(false)} />
      ) : null}
    </View>
  );
}

const BURGER_GAP = 4;
const TRAIL_GAP = 12;
const GUESS = 1040;
// Menu sheet metrics: a row's height, the sheet's padding and footer, and
// the card's gap under the bar.
const ITEM_HEIGHT = 54;
const SHEET_CHROME = 92;
const CARD_TOP = 8;

const styles = StyleSheet.create({
  bar: {
    position: 'absolute',
    left: 0,
    right: 0,
    top: 0,
    height: NAV_HEIGHT,
    flexDirection: 'row',
    alignItems: 'center',
    backgroundColor: 'rgba(5, 7, 15, 0.82)',
    borderBottomWidth: 1,
    borderBottomColor: site.stroke,
  },
  mark: {
    alignItems: 'center',
    justifyContent: 'center',
    experimental_backgroundImage: 'linear-gradient(135deg, #ffe29a 0%, #f2c35b 45%, #4fd1ff 100%)',
    boxShadow: '0 6px 20px rgba(242, 195, 91, 0.3)',
  },
  markInner: { backgroundColor: site.bg, transform: [{ rotate: '45deg' }] },
  word: { fontFamily: font, fontWeight: '700', letterSpacing: 3, color: colors.text },
  link: { height: 40, paddingHorizontal: 14, borderRadius: 10, justifyContent: 'center' },
  linkHot: { backgroundColor: 'rgba(255,255,255,0.05)' },
  linkActive: {},
  linkText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.textDim },
  sound: { height: 40, paddingHorizontal: 10, borderRadius: 10, flexDirection: 'row', alignItems: 'center', gap: 8 },
  eq: { width: 18, height: 16, flexDirection: 'row', alignItems: 'flex-end', justifyContent: 'space-between' },
  eqBar: { width: 3, borderRadius: 1.5 },
  eqSlash: { position: 'absolute', left: 8, top: -2, width: 2, height: 20, borderRadius: 1, backgroundColor: colors.textDim, transform: [{ rotate: '45deg' }] },
  main: { flex: 1, alignSelf: 'stretch' },
  // Wider than any bar, so the rows inside lay out at their natural width
  // (an absolute child is capped at its container's) and can be measured.
  ruler: { position: 'absolute', top: 0, bottom: 0, width: 4000, justifyContent: 'center' },
  cluster: { height: '100%', flexDirection: 'row', alignItems: 'center', gap: 2 },
  pinned: { position: 'absolute', right: 0, top: 0 },
  burger: {
    width: 42,
    height: 40,
    borderRadius: 10,
    alignItems: 'center',
    justifyContent: 'center',
    gap: 4,
    cursor: 'pointer',
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.08)',
    backgroundColor: 'rgba(255,255,255,0.03)',
  },
  burgerHot: { backgroundColor: 'rgba(255,255,255,0.08)', borderColor: 'rgba(255,255,255,0.16)' },
  burgerFocus: { borderColor: colors.gold },
  burgerBar: { width: 18, height: 2, borderRadius: 1, backgroundColor: colors.text },
  backdrop: { backgroundColor: 'rgba(2, 4, 10, 0.66)' },
  sheet: {
    paddingTop: 10,
    paddingBottom: 14,
    backgroundColor: '#070a14',
    borderBottomWidth: 1,
    borderBottomColor: site.stroke,
    borderBottomLeftRadius: 18,
    borderBottomRightRadius: 18,
    boxShadow: '0 24px 48px rgba(0, 0, 0, 0.55)',
    transformOrigin: 'top',
  },
  // Wider windows: a card hanging under the menu button.
  card: {
    alignSelf: 'flex-end',
    width: 380,
    marginTop: CARD_TOP,
    paddingHorizontal: 8,
    borderWidth: 1,
    borderColor: site.stroke,
    borderRadius: 18,
    transformOrigin: 'top right',
  },
  sheetGlow: {
    ...StyleSheet.absoluteFillObject,
    borderRadius: 18,
    experimental_backgroundImage: 'radial-gradient(ellipse at 0% 0%, rgba(242, 195, 91, 0.10) 0%, rgba(7, 10, 20, 0) 60%)',
  },
  sheetRows: { flexGrow: 0, flexShrink: 1 },
  grid: { flexDirection: 'row', flexWrap: 'wrap' },
  gridCell: { width: '50%' },
  item: { height: ITEM_HEIGHT, borderRadius: 12, paddingHorizontal: 14, flexDirection: 'row', alignItems: 'center', gap: 14, cursor: 'pointer' },
  itemActive: { backgroundColor: 'rgba(242, 195, 91, 0.07)' },
  itemHot: { backgroundColor: 'rgba(255,255,255,0.05)' },
  itemFocus: { borderWidth: 1, borderColor: colors.gold },
  itemBar: { position: 'absolute', left: 0, top: 14, bottom: 14, width: 3, borderRadius: 1.5, backgroundColor: colors.gold },
  itemNum: { fontFamily: mono, fontSize: 12, color: colors.textDim, width: 20 },
  itemText: { flex: 1, fontFamily: font, fontWeight: '700', fontSize: 19, letterSpacing: 0.2, color: colors.textDim },
  itemChevron: { fontFamily: font, fontSize: 24, color: colors.textDim, marginTop: -2 },
  sheetFooter: {
    marginTop: 10,
    marginHorizontal: 14,
    paddingTop: 14,
    borderTopWidth: 1,
    borderTopColor: site.stroke,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 12,
  },
  footerNote: { flex: 1, fontFamily: font, fontSize: 12, color: colors.textDim },
  underline: { position: 'absolute', left: 14, right: 14, bottom: 4, height: 2, borderRadius: 1, backgroundColor: colors.gold },
});
