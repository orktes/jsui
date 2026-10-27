// /examples and /examples/<id>: one live game screen on a big Stage, a
// selector for all of them, prev / next, what is interactive in the current
// screen and a link to its source. /examples/<id>/full plays it full screen
// (FullScreen). Only the selected screen is mounted, once.
//
// Input: the selector is focusable (Tab, arrows, D-pad). Pressing the
// selected entry again (Enter / A) moves focus into the screen; Esc / B inside
// the screen returns to the selector. [ and ] switch screens anywhere; LB / RB
// switch while focus is outside the screen (inside, they belong to the game).
import React, { memo, useCallback, useEffect, useRef, useState } from 'react';
import { Animated, Easing, Pressable, ScrollView, StyleSheet, Text, View, useGamepad, useMusic } from 'react-native';
import { Button, Column, Eyebrow, GITHUB, Key, Reveal, Stage, useLayout } from '../components/ui.jsx';
import { colors, font, site, text } from '../theme.js';
import { useRouter } from '../router.js';
import { SCREENS, screenById } from '../screens/index.js';
import { MUSIC, playUI } from '../sound.js';
import { FullScreen } from './examples/FullScreen.jsx';

// Music per screen (site/app/sound.js): the menu theme carries on between the
// main menu and its settings, the Rift has its own; switching crossfades, and
// the other screens fade it out.
const SCREEN_MUSIC = { 'main-menu': MUSIC.menu, settings: MUSIC.menu, rift: MUSIC.rift };

const SIDE_W = 360;
const GAP = 24;
const sourceUrl = (id) => `${GITHUB}/blob/main/site/app/screens/${id}.jsx`;
const pad2 = (n) => String(n).padStart(2, '0');

// --- selector entries ------------------------------------------------------------

const ListItem = memo(function ListItem({ s, i, selected, width, onPress, itemRef }) {
  // Focus ring only for keyboard / gamepad focus (like :focus-visible).
  const [visible, setVisible] = useState(false);
  return (
    <Pressable
      ref={itemRef}
      onPress={() => onPress(s.id)}
      onFocus={(e) => setVisible(!!e.nativeEvent.focusVisible)}
      onBlur={() => setVisible(false)}
      style={{ cursor: 'pointer', width }}
    >
      {({ hovered, focused: f, pressed }) => {
        const focused = f && visible;
        return (
        <View
          style={[
            styles.chip,
            hovered && styles.itemHover,
            selected && [styles.itemSelected, { borderColor: s.accent + '99' }],
            focused && styles.itemFocused,
            pressed && { transform: [{ scale: 0.97 }] },
          ]}
        >
          <View style={[styles.chipBar, { backgroundColor: selected ? s.accent : 'rgba(255,255,255,0.08)' }, selected && { boxShadow: `0 0 12px ${s.accent}` }]} />
          <View style={styles.chipTop}>
            <Text style={[styles.itemNum, selected && { color: s.accent }]}>{pad2(i + 1)}</Text>
            {selected ? (
              <View style={styles.live}>
                <View style={[styles.liveDot, { backgroundColor: s.accent }]} />
                <Text style={[styles.liveText, { color: s.accent }]}>Live</Text>
              </View>
            ) : null}
          </View>
          <Text style={[styles.itemTitle, (selected || hovered || focused) && { color: colors.text }]} numberOfLines={1}>
            {s.title}
          </Text>
          <Text style={styles.itemFeature} numberOfLines={2}>
            {s.feature ? s.feature.title : s.blurb}
          </Text>
        </View>
        );
      }}
    </Pressable>
  );
});

// --- the stage with a slide / crossfade between screens ------------------------------

function LiveStage({ id, width, mountKey, onEscape, padInside, onShown, away }) {
  const [shown, setShown] = useState({ id, from: 0 });
  const t = useRef(new Animated.Value(1)).current;
  const latest = useRef(id);
  latest.current = id;
  const [leaving, setLeaving] = useState(0); // -1 / 1 while sliding out

  useEffect(() => {
    if (id === shown.id) return;
    playUI('swipe', { volume: 0.55 });
    const dir = SCREENS.findIndex((s) => s.id === id) > SCREENS.findIndex((s) => s.id === shown.id) ? 1 : -1;
    setLeaving(dir);
    Animated.timing(t, { toValue: 0, duration: 170, easing: Easing.in(Easing.quad), useNativeDriver: true }).start(({ finished }) => {
      if (!finished) return;
      setLeaving(0);
      setShown({ id: latest.current, from: dir });
      setTimeout(onShown, 60); // after the new screen's autoFocus has run
      Animated.timing(t, { toValue: 1, duration: 420, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
    });
  }, [id]);

  const s = screenById(shown.id);
  const C = s.component;
  const dx = leaving ? -leaving * 60 : shown.from * 60;
  return (
    <View
      onKeyDown={(e) => {
        e.nativeEvent.inStage = true;
        if (e.key === 'Escape') {
          e.preventDefault();
          e.stopPropagation();
          playUI('back');
          onEscape();
        }
      }}
      onGamepadButton={(e) => {
        padInside.current = true;
        if (e.pressed && e.buttonIndex === 1) {
          e.preventDefault();
          e.stopPropagation();
          playUI('back');
          onEscape();
        }
      }}
    >
      <Stage width={width} style={{ backgroundColor: '#03050b' }}>
        <Animated.View
          style={{
            width: 1280,
            height: 720,
            opacity: t,
            transform: [{ translateX: t.interpolate({ inputRange: [0, 1], outputRange: [dx, 0] }) }, { scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.97, 1] }) }],
          }}
        >
          {away ? <Away s={s} /> : <C key={shown.id + ':' + mountKey} active />}
        </Animated.View>
      </Stage>
    </View>
  );
}

// In the stage's place while the screen plays full screen.
function Away({ s }) {
  return (
    <View style={[StyleSheet.absoluteFill, styles.away]}>
      <View style={[styles.awayGlow, { experimental_backgroundImage: `radial-gradient(ellipse at 50% 50%, ${s.accent}22 0%, rgba(0,0,0,0) 60%)` }]} />
      <Text style={[styles.awayEyebrow, { color: s.accent }]}>Playing full screen</Text>
      <Text style={styles.awayTitle}>{s.title}</Text>
    </View>
  );
}

// --- page ----------------------------------------------------------------------------

export function ScreensPage({ params = [] }) {
  const { go } = useRouter();
  const L = useLayout();
  const id = screenById(params[0]) ? params[0] : SCREENS[0].id;
  const full = params[1] === 'full';
  const idx = SCREENS.findIndex((s) => s.id === id);
  const s = SCREENS[idx];
  const wide = !L.tablet;
  const inner = Math.min(L.width - L.gutter * 2, 1240);
  const stageW = wide ? inner - SIDE_W - GAP : L.phone ? L.width - 20 : inner;
  // All cards in one row when they fit at 110 px, else a scrolling strip.
  const cardFit = (inner - (SCREENS.length - 1) * 10) / SCREENS.length;
  const cardW = cardFit >= 110 ? cardFit : 168;

  const itemRefs = useRef([]).current;
  const strip = useRef(null);
  const padInside = useRef(false);
  const [mountKey, setMountKey] = useState(0);
  useMusic(SCREEN_MUSIC[id] || null, { volume: 0.85, fadeIn: 1800, fadeOut: 1000 });

  // A switch made from outside the stage keeps focus on the selector: the
  // newly selected card takes it once the new screen is mounted (otherwise
  // the screen's autoFocus element would).
  const refocus = useRef(false);
  const idxRef = useRef(idx);
  idxRef.current = idx;
  const fullRef = useRef(full);
  fullRef.current = full;
  const select = useCallback((next) => {
    refocus.current = !fullRef.current;
    go('/examples/' + next + (fullRef.current ? '/full' : ''));
  }, [go]);
  const step = useCallback((d, inside) => {
    select(SCREENS[(idxRef.current + d + SCREENS.length) % SCREENS.length].id);
    if (inside) refocus.current = false;
  }, [select]);
  // Full screen mounts the screen afresh, so its autoFocus element takes focus.
  const openFull = useCallback(() => {
    setMountKey((k) => k + 1);
    go('/examples/' + id + '/full');
  }, [go, id]);
  const closeFull = useCallback(() => {
    refocus.current = true;
    setMountKey((k) => k + 1);
    go('/examples/' + id);
    setTimeout(onShown, 60);
  }, [go, id]);
  const onShown = useCallback(() => {
    if (!refocus.current) return;
    refocus.current = false;
    const r = itemRefs[idxRef.current];
    if (r) r.focus();
  }, []);
  const enter = useCallback(() => {
    // Nothing focused + a fresh mount = the screen's autoFocus element takes focus.
    const r = itemRefs[idx];
    if (r) r.blur();
    setMountKey((k) => k + 1);
  }, [idx]);
  const onItem = useCallback((target) => (target === id ? enter() : select(target)), [id, enter, select]);
  const backToSelector = useCallback(() => {
    const r = itemRefs[idx];
    if (r) r.focus();
  }, [idx]);

  // LB / RB from anywhere outside the stage (the stage marks its own events).
  useGamepad({
    onButton: (e) => {
      const inside = padInside.current;
      padInside.current = false;
      if (inside || !e.pressed || e.isDefaultPrevented()) return;
      if (fullRef.current && e.buttonIndex === 1) return; // B closes full screen (its Modal)
      if (e.buttonIndex === 3 && !fullRef.current) openFull(), e.preventDefault();
      else if (e.buttonIndex === 4) step(-1), e.preventDefault();
      else if (e.buttonIndex === 5) step(1), e.preventDefault();
    },
  });

  // Keep the selected card visible when the strip scrolls.
  useEffect(() => {
    if (cardFit < 110 && strip.current) strip.current.scrollTo({ x: Math.max(0, idx * (cardW + 10) - 40), animated: true });
  }, [idx, cardFit < 110]);

  const onKeyDown = (e) => {
    if (e.key === '[' || e.key === ']') {
      step(e.key === '[' ? -1 : 1, !!e.nativeEvent.inStage);
      e.preventDefault();
    } else if ((e.key === 'f' || e.key === 'F') && !e.nativeEvent.inStage) {
      openFull();
      e.preventDefault();
    }
  };

  const cards = SCREENS.map((sc, i) => (
    <ListItem key={sc.id} s={sc} i={i} selected={sc.id === id} width={cardW} onPress={onItem} itemRef={(r) => (itemRefs[i] = r)} />
  ));

  return (
    <View onKeyDown={onKeyDown} style={{ paddingBottom: 80 }}>
      <Column style={{ paddingTop: L.phone ? 32 : 56 }}>
        <Reveal>
          <View style={[styles.head, !wide && { flexDirection: 'column', alignItems: 'flex-start' }]}>
            <View style={{ flex: wide ? 1 : undefined, gap: 12, maxWidth: 740 }}>
              <Eyebrow>Examples · {SCREENS.length} live game UIs</Eyebrow>
              <Text style={[L.phone ? text.h2 : text.h1]}>Real game screens, running live.</Text>
              <Text style={[text.lead, L.phone && { fontSize: 16, lineHeight: 25 }]}>
                Not videos or screenshots: each screen is a React Native component laid out at 1280 × 720 and scaled onto the stage, and each one
                spotlights a different part of JSUI. Click into it, tab into it, or pick up a controller. Or open it full screen.
              </Text>
            </View>
            {L.phone ? null : (
              <View style={styles.legend}>
                <Legend k="[ ]" alt="LB RB" label="Switch screen" />
                <Legend k="Enter" alt="A" label="Play the selected screen" />
                <Legend k="Esc" alt="B" label="Back to the list" />
                <Legend k="F" alt="Y" label="Full screen" />
              </View>
            )}
          </View>
        </Reveal>
      </Column>

      <Column style={{ marginTop: L.phone ? 24 : 36 }}>
        <View style={[styles.toolbar, { width: wide ? stageW : '100%' }]}>
          <View style={[styles.livePill, { borderColor: s.accent + '55' }]}>
            <View style={[styles.liveDot, { backgroundColor: s.accent }]} />
            <Text style={[styles.liveText, { color: s.accent }]}>Live</Text>
          </View>
          <Text style={styles.toolbarTitle} numberOfLines={1}>
            {s.title}
          </Text>
          <Text style={styles.toolbarCount}>
            {pad2(idx + 1)} / {pad2(SCREENS.length)}
          </Text>
          <View style={{ flex: 1 }} />
          <FullButton compact={L.phone} onPress={openFull} />
          <ArrowButton label="‹" onPress={() => step(-1, true)} />
          <ArrowButton label="›" onPress={() => step(1, true)} />
        </View>
        <View style={{ flexDirection: wide ? 'row' : 'column', gap: GAP, alignItems: wide ? 'stretch' : 'center' }}>
          <LiveStage id={id} width={stageW} mountKey={mountKey} onEscape={backToSelector} padInside={padInside} onShown={onShown} away={full} />
          {wide ? <Spotlight s={s} style={{ width: SIDE_W }} /> : null}
        </View>

        <View style={{ marginTop: 18 }}>
          {cardFit >= 110 ? (
            <View style={{ flexDirection: 'row', gap: 10 }}>{cards}</View>
          ) : (
            <ScrollView
              ref={strip}
              horizontal
              showsHorizontalScrollIndicator={false}
              style={{ marginHorizontal: -L.gutter }}
              contentContainerStyle={{ paddingHorizontal: L.gutter, gap: 10 }}
            >
              {cards}
            </ScrollView>
          )}
        </View>

        {wide ? null : <Spotlight s={s} style={{ marginTop: 20 }} />}
      </Column>
      {full ? (
        <FullScreen s={s} index={idx} total={SCREENS.length} mountKey={mountKey} padInside={padInside} onStep={(d) => step(d, true)} onClose={closeFull} />
      ) : null}
    </View>
  );
}

function FullButton({ compact, onPress }) {
  return (
    <Pressable
      onPress={onPress}
      focusable={false}
      accessibilityLabel="Play full screen"
      style={styles.fullBtn}
      hoveredStyle={styles.arrowHot}
      pressedStyle={{ transform: [{ scale: 0.95 }] }}
      transition={{ duration: 120, pressed: 40 }}
    >
      <View style={styles.fullIcon}>
        <View style={[styles.corner, { left: 0, top: 0, borderLeftWidth: 2, borderTopWidth: 2 }]} />
        <View style={[styles.corner, { right: 0, top: 0, borderRightWidth: 2, borderTopWidth: 2 }]} />
        <View style={[styles.corner, { left: 0, bottom: 0, borderLeftWidth: 2, borderBottomWidth: 2 }]} />
        <View style={[styles.corner, { right: 0, bottom: 0, borderRightWidth: 2, borderBottomWidth: 2 }]} />
      </View>
      {compact ? null : <Text style={styles.fullText}>Full screen</Text>}
    </Pressable>
  );
}

function Legend({ k, alt, label }) {
  return (
    <View style={styles.legendRow}>
      <Key>{k}</Key>
      <Key color={colors.cyan}>{alt}</Key>
      <Text style={styles.legendText}>{label}</Text>
    </View>
  );
}

function ArrowButton({ label, onPress }) {
  return (
    <Pressable onPress={onPress} focusable={false} style={{ cursor: 'pointer' }}>
      {({ hovered, pressed }) => (
        <View style={[styles.arrow, hovered && styles.arrowHot, pressed && { transform: [{ scale: 0.92 }] }]}>
          <Text style={styles.arrowText}>{label}</Text>
        </View>
      )}
    </Pressable>
  );
}

/** What the current screen demonstrates, its controls and a source link. */
function Spotlight({ s, style }) {
  const fade = useRef(new Animated.Value(1)).current;
  useEffect(() => {
    fade.setValue(0);
    Animated.timing(fade, { toValue: 1, duration: 420, delay: 140, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, [s.id]);
  const f = s.feature;
  return (
    <View style={[styles.spot, { borderColor: s.accent + '44' }, style]}>
      <View style={[styles.spotGlow, { experimental_backgroundImage: `radial-gradient(ellipse at 0% 0%, ${s.accent}2e 0%, rgba(0,0,0,0) 70%)` }]} />
      <Animated.View style={{ flex: 1, gap: 14, opacity: fade, transform: [{ translateY: fade.interpolate({ inputRange: [0, 1], outputRange: [10, 0] }) }] }}>
        <Eyebrow color={s.accent}>Feature spotlight</Eyebrow>
        <Text style={styles.spotTitle}>{f ? f.title : s.title}</Text>
        <Text style={styles.spotBlurb}>{s.blurb}</Text>
        {f && f.points ? (
          <View style={{ gap: 9 }}>
            {f.points.map((p) => (
              <View key={p} style={styles.feature}>
                <View style={[styles.bullet, { backgroundColor: s.accent }]} />
                <Text style={styles.point}>{p}</Text>
              </View>
            ))}
          </View>
        ) : null}
        {f && f.keys && f.keys.length ? (
          <View style={styles.controls}>
            {f.keys.map((k) => {
              const at = k.indexOf(': ');
              return (
                <View key={k} style={styles.controlRow}>
                  {at > 0 ? (
                    <>
                      <Text style={styles.controlKey}>{k.slice(0, at)}</Text>
                      <Text style={styles.controlAction}>{k.slice(at + 2)}</Text>
                    </>
                  ) : (
                    <Text style={styles.controlAction}>{k}</Text>
                  )}
                </View>
              );
            })}
          </View>
        ) : null}
        <View style={{ flex: 1 }} />
        <Button small kind="ghost" icon="</>" label={`View source · ${s.id}.jsx`} href={sourceUrl(s.id)} />
      </Animated.View>
    </View>
  );
}

const styles = StyleSheet.create({
  head: { flexDirection: 'row', alignItems: 'flex-end', gap: 32 },
  legend: { gap: 8, paddingBottom: 6 },
  legendRow: { flexDirection: 'row', alignItems: 'center', gap: 6 },
  legendText: { fontFamily: font, fontSize: 13, color: colors.textDim, marginLeft: 4 },

  toolbar: { flexDirection: 'row', alignItems: 'center', gap: 12, height: 38, marginBottom: 12 },
  toolbarTitle: { fontFamily: font, fontWeight: '700', fontSize: 17, color: colors.text, flexShrink: 1 },
  toolbarCount: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textMute },
  livePill: { flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 9, height: 24, borderRadius: 12, borderWidth: 1 },
  liveDot: { width: 7, height: 7, borderRadius: 4 },
  liveText: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.4, textTransform: 'uppercase' },
  arrow: { width: 36, height: 36, borderRadius: 10, borderWidth: 1, borderColor: site.strokeStrong, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(255,255,255,0.03)' },
  arrowHot: { borderColor: 'rgba(79,209,255,0.6)', backgroundColor: 'rgba(79,209,255,0.08)' },
  fullBtn: {
    height: 36,
    paddingHorizontal: 11,
    borderRadius: 10,
    borderWidth: 1,
    borderColor: site.strokeStrong,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 9,
    backgroundColor: 'rgba(255,255,255,0.03)',
    cursor: 'pointer',
  },
  fullIcon: { width: 14, height: 14 },
  corner: { position: 'absolute', width: 5, height: 5, borderColor: colors.text },
  fullText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },
  away: { alignItems: 'center', justifyContent: 'center', gap: 14, backgroundColor: '#03050b' },
  awayGlow: { ...StyleSheet.absoluteFillObject },
  awayEyebrow: { fontFamily: font, fontWeight: '700', fontSize: 22, letterSpacing: 4, textTransform: 'uppercase' },
  awayTitle: { fontFamily: font, fontWeight: '700', fontSize: 64, letterSpacing: -1, color: colors.text },
  arrowText: { fontFamily: font, fontWeight: '600', fontSize: 22, lineHeight: 24, color: colors.text, marginTop: -3 },

  spot: { padding: 22, borderRadius: 18, borderWidth: 1, backgroundColor: site.card, overflow: 'hidden', boxShadow: '0 24px 60px rgba(0,0,0,0.45)' },
  spotGlow: { ...StyleSheet.absoluteFillObject },
  spotTitle: { fontFamily: font, fontWeight: '700', fontSize: 22, lineHeight: 28, color: colors.text, letterSpacing: -0.2 },
  spotBlurb: { fontFamily: font, fontSize: 14, lineHeight: 21, color: colors.textDim },
  point: { flex: 1, fontFamily: font, fontSize: 13.5, lineHeight: 20, color: colors.textDim },
  chipTop: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', height: 20 },
  chipBar: { position: 'absolute', left: 10, right: 10, top: -1, height: 2, borderRadius: 1 },
  itemFeature: { fontFamily: font, fontSize: 11.5, lineHeight: 16, color: colors.textMute, marginTop: 2 },
  item: {
    flexDirection: 'row',
    alignItems: 'flex-start',
    gap: 12,
    paddingVertical: 11,
    paddingHorizontal: 14,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: 'transparent',
  },
  chip: {
    padding: 12,
    paddingTop: 10,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: site.stroke,
    backgroundColor: site.card,
    minHeight: 92,
  },
  itemHover: { backgroundColor: 'rgba(255,255,255,0.04)' },
  itemSelected: { backgroundColor: 'rgba(16,22,40,0.95)', boxShadow: '0 16px 40px rgba(0,0,0,0.45)' },
  itemFocused: { boxShadow: '0 0 0 2px #05070f, 0 0 0 4px rgba(79, 209, 255, 0.9)' },
  itemBar: { position: 'absolute', left: -1, top: 12, bottom: 12, width: 3, borderRadius: 2 },
  itemNum: { fontFamily: 'JetBrains Mono', fontWeight: '700', fontSize: 12, color: colors.textMute, marginTop: 3, width: 20 },
  itemTitle: { fontFamily: font, fontWeight: '600', fontSize: 15, color: colors.textDim },
  itemBlurb: { fontFamily: font, fontSize: 12.5, lineHeight: 18, color: colors.textMute, marginTop: 2 },
  live: { flexDirection: 'row', alignItems: 'center', gap: 5 },

  details: {
    marginTop: 28,
    gap: 32,
    padding: 24,
    borderRadius: 18,
    borderWidth: 1,
    borderColor: site.stroke,
    backgroundColor: site.card,
  },
  feature: { flexDirection: 'row', gap: 12, alignItems: 'flex-start' },
  bullet: { width: 6, height: 6, borderRadius: 1, marginTop: 9, transform: [{ rotate: '45deg' }] },
  controls: { borderRadius: 12, borderWidth: 1, borderColor: site.stroke, overflow: 'hidden' },
  controlRow: { flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 12, paddingVertical: 8, borderBottomWidth: 1, borderBottomColor: site.stroke },
  controlKey: { width: 118, fontFamily: 'JetBrains Mono', fontWeight: '700', fontSize: 12, color: colors.gold },
  controlAction: { flex: 1, fontFamily: font, fontSize: 13, lineHeight: 18, color: colors.textDim },
});
