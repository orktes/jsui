// Interactive title screen (1280×720): the examples/screens/main-menu.jsx
// reference brought to life. Drifting backdrop with pointer parallax and
// rising embers, staggered entrance, a menu stack whose selection follows
// hover / focus (keyboard, gamepad D-pad) with a sliding focus bar, an
// auto-advancing news carousel, toasts and a quit confirmation.
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Image, LayoutAnimation, Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font, type } from '../../../examples/theme.js';

export const feature = {
  title: 'Spatial focus + spring feedback',
  points: [
    'One selection model for every input: hover, Tab / arrows and the gamepad D-pad all move the same focus through the menu stack.',
    'Hover, press and focus drive Animated springs on the native driver; the gold focus bar glides between rows while LayoutAnimation resizes them.',
    'A modal confirm traps focus (accessibilityViewIsModal) and hands it back on close; Escape / B dismiss it.',
    'Ambient motion (backdrop drift, pointer parallax, embers, auto-advancing news) runs as native loops and pauses when the screen is inactive.',
  ],
  keys: ['Mouse: hover to select, click to activate', 'Arrows / D-pad: move focus', 'Enter / A: select', 'Y: patch notes', 'Esc / B: close the quit dialog', '← → on the news card: change story'],
};

const ND = true; // native driver for opacity / transform

const ITEMS = [
  { id: 'continue', label: 'Continue', hint: 'Act III · The Ember Gate', toast: 'Loading Act III · The Ember Gate' },
  { id: 'new', label: 'New Journey', hint: 'Begin a fresh chronicle', toast: 'Choose your origin' },
  { id: 'mp', label: 'Multiplayer', hint: '5 friends online', badge: '3 invites', toast: 'Joining party lobby' },
  { id: 'armory', label: 'Armory', hint: '2 new items unlocked', toast: 'Opening the Armory' },
  { id: 'settings', label: 'Settings', hint: 'Video, audio and controls', toast: 'Opening settings' },
  { id: 'quit', label: 'Quit to Desktop', hint: 'Progress saved 2 min ago', muted: true },
];
const ROW = 48;
const ROW_SEL = 64;
const GAP = 8;

const NEWS = [
  {
    art: require('../../../examples/assets/card-ember.png'),
    tag: 'New',
    tint: colors.orange,
    kicker: 'Patch 4.2 · Live now',
    title: 'The Ember Tide rises',
    text: "Three new rifts, the Cinderforged armor set and a reworked talent tree for every class. Log in before the tide recedes to claim the Founder's Brazier.",
  },
  {
    art: require('../../../examples/assets/card-tide.png'),
    tag: 'Event',
    tint: colors.cyan,
    kicker: 'Weekend event · 2 days left',
    title: 'Tidecaller Regatta',
    text: 'Race the drowned isles with your crew. Every finish earns Abyssal shards toward the Leviathan mount.',
  },
  {
    art: require('../../../examples/assets/card-void.png'),
    tag: 'Ranked',
    tint: colors.purple,
    kicker: 'Season 4 · Ranked',
    title: 'Voidbreaker Trials',
    text: 'Climb the ladder before the season ends. The top 500 champions are crowned with the Void Diadem.',
  },
];
const NEWS_MS = 5200;

const EMBERS = Array.from({ length: 16 }, (_, i) => ({
  x: 520 + ((i * 197) % 740),
  size: 2 + (i % 3),
  dur: 6000 + ((i * 1373) % 5000),
  delay: (i * 811) % 6000,
  drift: ((i % 5) - 2) * 18,
  hue: i % 4 === 0 ? '#ffe29a' : i % 3 === 0 ? '#ff9a3c' : '#ffb85c',
}));

const useValue = (v) => useRef(new Animated.Value(v)).current;
const spring = (v, toValue, cfg) => Animated.spring(v, Object.assign({ toValue, stiffness: 320, damping: 22, mass: 0.9, useNativeDriver: ND }, cfg));

// ---------------------------------------------------------------------------

const MenuButton = memo(function MenuButton({ item, index, selected, enter, onSelect, onActivate, autoFocus }) {
  const sel = useValue(selected ? 1 : 0);
  const press = useValue(0);
  useEffect(() => {
    spring(sel, selected ? 1 : 0, { stiffness: 260, damping: 20 }).start();
  }, [selected]);
  const translateX = Animated.add(
    enter.interpolate({ inputRange: [0, 1], outputRange: [-60, 0] }),
    sel.interpolate({ inputRange: [0, 1], outputRange: [0, 10] }),
  );
  const scale = press.interpolate({ inputRange: [0, 1], outputRange: [1, 0.96] });
  return (
    <Pressable
      autoFocus={autoFocus}
      onHoverIn={() => onSelect(index)}
      onFocus={() => onSelect(index)}
      onPressIn={() => spring(press, 1, { stiffness: 700, damping: 30 }).start()}
      onPressOut={() => spring(press, 0, { stiffness: 300, damping: 12 }).start()}
      onPress={() => onActivate(index)}
      style={{ cursor: 'pointer' }}
    >
      <Animated.View style={[styles.button, selected && styles.buttonSelectedShape, { opacity: enter, transform: [{ translateX }, { scale }] }]}>
        <Animated.View style={[StyleSheet.absoluteFill, styles.buttonGlow, { opacity: sel }]} />
        <View style={{ flex: 1 }}>
          <Text style={[styles.buttonText, selected && styles.buttonTextFocused, item.muted && !selected && { color: colors.textMute }]}>{item.label}</Text>
          {selected ? <Text style={styles.buttonHint}>{item.hint}</Text> : null}
        </View>
        {item.badge ? (
          <View style={styles.badge}>
            <Text style={styles.badgeText}>{item.badge}</Text>
          </View>
        ) : null}
        <Animated.Text
          style={[styles.chevron, { opacity: sel, transform: [{ translateX: sel.interpolate({ inputRange: [0, 1], outputRange: [-12, 0] }) }] }]}
        >
          ›
        </Animated.Text>
      </Animated.View>
    </Pressable>
  );
});

function KeyHint({ glyph, label, color, flash }) {
  return (
    <View style={styles.keyHint}>
      <Animated.View style={[styles.keyGlyph, { borderColor: color, transform: [{ scale: flash.interpolate({ inputRange: [0, 1], outputRange: [1, 1.25] }) }] }]}>
        <Animated.View style={[StyleSheet.absoluteFill, { borderRadius: 12, backgroundColor: color, opacity: flash.interpolate({ inputRange: [0, 1], outputRange: [0, 0.45] }) }]} />
        <Text style={[styles.keyGlyphText, { color }]}>{glyph}</Text>
      </Animated.View>
      <Text style={styles.keyLabel}>{label}</Text>
    </View>
  );
}

function Ember({ e, active }) {
  const t = useValue(0);
  useEffect(() => {
    if (!active) return undefined;
    const anim = Animated.loop(Animated.timing(t, { toValue: 1, duration: e.dur, easing: Easing.linear, useNativeDriver: ND }));
    const timer = setTimeout(() => anim.start(), e.delay);
    return () => {
      clearTimeout(timer);
      anim.stop();
    };
  }, [active]);
  return (
    <Animated.View
      pointerEvents="none"
      style={{
        position: 'absolute',
        left: e.x,
        top: 720,
        width: e.size,
        height: e.size,
        borderRadius: e.size,
        backgroundColor: e.hue,
        boxShadow: `0 0 ${e.size * 3}px ${e.hue}`,
        opacity: t.interpolate({ inputRange: [0, 0.15, 0.7, 1], outputRange: [0, 0.9, 0.6, 0] }),
        transform: [
          { translateY: t.interpolate({ inputRange: [0, 1], outputRange: [0, -560] }) },
          { translateX: t.interpolate({ inputRange: [0, 0.5, 1], outputRange: [0, e.drift, e.drift * 0.2] }) },
        ],
      }}
    />
  );
}

function NewsCard({ active, enter }) {
  const [idx, setIdx] = useState(0);
  const [paused, setPaused] = useState(false);
  const pos = useValue(0);
  const timer = useValue(0);
  const hover = useValue(0);
  const idxRef = useRef(0);

  const goTo = useCallback((next, dir) => {
    const n = NEWS.length;
    const cur = idxRef.current;
    const target = ((next % n) + n) % n;
    if (target === cur) return;
    idxRef.current = target;
    LayoutAnimation.configureNext(LayoutAnimation.create(320, 'easeInEaseOut', 'opacity'));
    setIdx(target);
    // Wrap so the carousel always slides in the travel direction.
    let to = target;
    if (dir > 0 && target < cur) to = cur + 1;
    if (dir < 0 && target > cur) to = cur - 1;
    Animated.timing(pos, { toValue: to, duration: 520, easing: Easing.out(Easing.cubic), useNativeDriver: ND }).start(({ finished }) => {
      if (finished && to !== target) pos.setValue(target);
    });
  }, []);

  // Auto-advance: the active pip fills up, then the next story slides in.
  useEffect(() => {
    if (!active || paused) {
      timer.stopAnimation();
      return undefined;
    }
    timer.setValue(0);
    const anim = Animated.timing(timer, { toValue: 1, duration: NEWS_MS, easing: Easing.linear, useNativeDriver: ND });
    anim.start(({ finished }) => finished && goTo(idxRef.current + 1, 1));
    return () => anim.stop();
  }, [active, paused, idx]);

  const n = NEWS.length;
  return (
    <Animated.View
      style={[
        styles.newsWrap,
        { opacity: enter, transform: [{ translateX: enter.interpolate({ inputRange: [0, 1], outputRange: [60, 0] }) }] },
      ]}
    >
      <Pressable
        onHoverIn={() => {
          setPaused(true);
          spring(hover, 1).start();
        }}
        onHoverOut={() => {
          setPaused(false);
          spring(hover, 0).start();
        }}
        onFocus={() => setPaused(true)}
        onBlur={() => setPaused(false)}
        onPress={() => goTo(idxRef.current + 1, 1)}
        onKeyDown={(e) => {
          if (e.key === 'ArrowRight') {
            goTo(idxRef.current + 1, 1);
            e.preventDefault();
          } else if (e.key === 'ArrowLeft') {
            goTo(idxRef.current - 1, -1);
            e.preventDefault();
          }
        }}
        style={{ cursor: 'pointer' }}
        group
      >
        <Animated.View
          style={[styles.news, { transform: [{ translateY: hover.interpolate({ inputRange: [0, 1], outputRange: [0, -4] }) }] }]}
          groupFocusedStyle={styles.newsFocused}
        >
          <View style={styles.newsArtClip}>
            {NEWS.map((it, i) => {
              const range = [i - 4, i - 3, i - 2, i - 1, i, i + 1, i + 2, i + 3, i + 4];
              return (
                <Animated.View
                  key={i}
                  style={[
                    StyleSheet.absoluteFill,
                    {
                      opacity: pos.interpolate({ inputRange: range, outputRange: [0, 1, 0, 0, 1, 0, 0, 1, 0] }),
                      transform: [
                        { translateX: pos.interpolate({ inputRange: range, outputRange: [-60, 0, 60, 60, 0, -60, -60, 0, 60] }) },
                        { scale: hover.interpolate({ inputRange: [0, 1], outputRange: [1.04, 1.1] }) },
                      ],
                    },
                  ]}
                >
                  <Image source={it.art} style={styles.newsArt} resizeMode="cover" />
                </Animated.View>
              );
            })}
            <View style={styles.newsArtFade} />
            <View style={[styles.newsTag, { backgroundColor: NEWS[idx].tint }]}>
              <Text style={styles.newsTagText}>{NEWS[idx].tag}</Text>
            </View>
            <View style={styles.newsCount}>
              <Text style={styles.newsCountText}>
                {idx + 1} / {n}
              </Text>
            </View>
          </View>
          <View style={styles.newsBody}>
            <View style={{ height: 102 }}>
              {NEWS.map((it, i) => {
                const range = [i - 4, i - 3, i - 2, i - 1, i, i + 1, i + 2, i + 3, i + 4];
                return (
                  <Animated.View
                    key={i}
                    pointerEvents="none"
                    style={[
                      StyleSheet.absoluteFill,
                      {
                        opacity: pos.interpolate({ inputRange: range, outputRange: [0, 1, 0, 0, 1, 0, 0, 1, 0] }),
                        transform: [{ translateX: pos.interpolate({ inputRange: range, outputRange: [-24, 0, 24, 24, 0, -24, -24, 0, 24] }) }],
                      },
                    ]}
                  >
                    <Text style={[styles.newsKicker, { color: it.tint }]}>{it.kicker}</Text>
                    <Text style={styles.newsTitle}>{it.title}</Text>
                    <Text style={styles.newsText} numberOfLines={2}>
                      {it.text}
                    </Text>
                  </Animated.View>
                );
              })}
            </View>
            <View style={styles.pips}>
              {NEWS.map((it, i) => (
                <Pressable key={i} focusable={false} onPress={() => goTo(i, i > idx ? 1 : -1)} hitSlop={6} style={{ cursor: 'pointer' }}>
                  <View style={[styles.pip, i === idx && styles.pipActive]}>
                    {i === idx ? (
                      <Animated.View
                        style={[
                          styles.pipFill,
                          { backgroundColor: NEWS[idx].tint, transform: [{ scaleX: paused || !active ? 1 : timer }] },
                        ]}
                      />
                    ) : null}
                  </View>
                </Pressable>
              ))}
              <View style={{ flex: 1 }} />
              <Text style={styles.newsMore}>{paused ? 'Paused' : 'Read more'}</Text>
            </View>
          </View>
        </Animated.View>
      </Pressable>
    </Animated.View>
  );
}

function Toast({ toast }) {
  const t = useValue(0);
  const bar = useValue(0);
  useEffect(() => {
    if (!toast) return undefined;
    t.setValue(0);
    bar.setValue(0);
    spring(t, 1, { stiffness: 260, damping: 18 }).start();
    Animated.timing(bar, { toValue: 1, duration: 2000, easing: Easing.inOut(Easing.quad), useNativeDriver: ND }).start();
    const id = setTimeout(() => Animated.timing(t, { toValue: 0, duration: 260, useNativeDriver: ND }).start(), 2300);
    return () => clearTimeout(id);
  }, [toast]);
  return (
    <Animated.View
      pointerEvents="none"
      style={[
        styles.toast,
        { opacity: t, transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [-24, 0] }) }, { scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.94, 1] }) }] },
      ]}
    >
      <View style={styles.toastSpinner}>
        <Animated.View style={[styles.toastDiamond, { transform: [{ rotate: bar.interpolate({ inputRange: [0, 1], outputRange: ['45deg', '405deg'] }) }] }]} />
      </View>
      <View style={{ flex: 1 }}>
        <Text style={styles.toastText}>{toast ? toast.text : ''}</Text>
        <View style={styles.toastTrack}>
          <Animated.View style={[styles.toastFill, { transform: [{ scaleX: bar }] }]} />
        </View>
      </View>
    </Animated.View>
  );
}

function QuitDialog({ onCancel, onQuit }) {
  const t = useValue(0);
  useEffect(() => {
    spring(t, 1, { stiffness: 280, damping: 20 }).start();
  }, []);
  const close = (fn) => Animated.timing(t, { toValue: 0, duration: 160, useNativeDriver: ND }).start(() => fn());
  return (
    <Animated.View
      accessibilityViewIsModal
      style={[styles.dialogScrim, { opacity: t }]}
      onKeyDown={(e) => {
        if (e.key === 'Escape') {
          close(onCancel);
          e.preventDefault();
          e.stopPropagation();
        }
      }}
      onGamepadButton={(e) => {
        if (e.buttonIndex === 1) {
          if (e.pressed) close(onCancel);
          e.preventDefault();
          e.stopPropagation();
        }
      }}
    >
      <Animated.View style={[styles.dialog, { transform: [{ scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.9, 1] }) }] }]}>
        <Text style={styles.dialogKicker}>Leaving so soon?</Text>
        <Text style={styles.dialogTitle}>Quit to desktop</Text>
        <Text style={styles.dialogText}>Your progress was saved 2 minutes ago at the Ember Gate. Unsaved party invites will expire.</Text>
        <View style={styles.dialogRow}>
          <DialogButton label="Stay" glyph="B" glyphColor="#ff6b81" onPress={() => close(onCancel)} autoFocus />
          <DialogButton label="Quit" glyph="A" glyphColor="#5fe08a" danger onPress={() => close(onQuit)} />
        </View>
      </Animated.View>
    </Animated.View>
  );
}

function DialogButton({ label, glyph, glyphColor, onPress, autoFocus, danger }) {
  return (
    <Pressable autoFocus={autoFocus} onPress={onPress} group style={{ flex: 1, cursor: 'pointer' }}>
      <View
        style={[styles.dialogBtn, danger && styles.dialogBtnDanger]}
        groupHoveredStyle={danger ? styles.dialogBtnDangerHot : styles.dialogBtnHot}
        groupFocusedStyle={danger ? styles.dialogBtnDangerHot : styles.dialogBtnHot}
        groupPressedStyle={styles.pressed96}
      >
        <View style={[styles.keyGlyph, { borderColor: glyphColor, width: 22, height: 22 }]}>
          <Text style={[styles.keyGlyphText, { color: glyphColor }]}>{glyph}</Text>
        </View>
        <Text style={styles.dialogBtnText}>{label}</Text>
      </View>
    </Pressable>
  );
}

// ---------------------------------------------------------------------------

// `autoFocus` (default: `active`) is off where the screen is a preview on a
// longer page: taking focus at load would scroll the page to it on a phone.
export default function MainMenu({ active = true, autoFocus = active }) {
  const [sel, setSel] = useState(0);
  const [toast, setToast] = useState(null);
  const [confirm, setConfirm] = useState(false);
  // The ping readout is a native value: a new reading every 2.4 s is a
  // setValue, not a render of the whole screen.
  const ping = useRef(new Animated.Value(38, { useNativeDriver: true })).current;
  const pingText = useMemo(() => Animated.format(ping, { decimals: 0, prefix: 'v4.2.0 · EU-West · ', suffix: ' ms' }), [ping]);
  const pingColor = useMemo(() => ping.interpolate({ inputRange: [40, 41], outputRange: [colors.green, colors.orange], extrapolate: 'clamp' }), [ping]);

  const intro = useValue(active ? 0 : 1);
  const items = useRef(ITEMS.map(() => new Animated.Value(active ? 0 : 1))).current;
  const news = useValue(active ? 0 : 1);
  const drift = useValue(0);
  const glow = useValue(0);
  const px = useValue(0);
  const py = useValue(0);
  const flash = useRef({ A: new Animated.Value(0), B: new Animated.Value(0), Y: new Animated.Value(0) }).current;
  const barY = useValue(10);

  // Staggered entrance (skipped for thumbnails that start inactive).
  useEffect(() => {
    if (!active) return;
    Animated.parallel([
      Animated.timing(intro, { toValue: 1, duration: 900, easing: Easing.out(Easing.cubic), useNativeDriver: ND }),
      Animated.stagger(
        70,
        items.map((v) => Animated.timing(v, { toValue: 1, duration: 520, easing: Easing.out(Easing.back(1.4)), useNativeDriver: ND })),
      ),
      Animated.timing(news, { toValue: 1, duration: 700, delay: 500, easing: Easing.out(Easing.cubic), useNativeDriver: ND }),
    ]).start();
  }, []);

  // Ambient loops: slow Ken Burns drift on the backdrop, pulsing title glow, ping jitter.
  useEffect(() => {
    if (!active) return undefined;
    const loops = [
      Animated.loop(
        Animated.sequence([
          Animated.timing(drift, { toValue: 1, duration: 14000, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
          Animated.timing(drift, { toValue: 0, duration: 14000, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
        ]),
      ),
      Animated.loop(
        Animated.sequence([
          Animated.timing(glow, { toValue: 1, duration: 2200, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
          Animated.timing(glow, { toValue: 0, duration: 2200, easing: Easing.inOut(Easing.sin), useNativeDriver: ND }),
        ]),
      ),
    ];
    loops.forEach((l) => l.start());
    const id = setInterval(() => ping.setValue(34 + Math.round(Math.random() * 9)), 2400);
    return () => {
      loops.forEach((l) => l.stop());
      clearInterval(id);
    };
  }, [active]);

  // The gold focus bar slides to the selected row.
  useEffect(() => {
    spring(barY, sel * (ROW + GAP) + 10, { stiffness: 300, damping: 24 }).start();
  }, [sel]);

  const select = useCallback((i) => {
    setSel((cur) => {
      if (cur !== i) LayoutAnimation.configureNext(LayoutAnimation.create(220, 'easeInEaseOut', 'opacity'));
      return i;
    });
  }, []);

  const pulse = (k) => {
    flash[k].setValue(1);
    Animated.timing(flash[k], { toValue: 0, duration: 420, easing: Easing.out(Easing.quad), useNativeDriver: ND }).start();
  };

  const activate = useCallback((i) => {
    pulse('A');
    const it = ITEMS[i];
    if (it.id === 'quit') setConfirm(true);
    else setToast({ text: it.toast, at: Date.now() });
  }, []);

  // Pointer parallax: normalise the pointer against the root's on-screen rect
  // (the screen may be scaled by a <Stage>).
  const rootRef = useRef(null);
  const rect = useRef(null);
  const measure = () => {
    const r = rootRef.current;
    if (r && r.measureInWindow) r.measureInWindow((x, y, w, h) => (rect.current = { x, y, w, h }));
  };
  const onPointerMove = (e) => {
    const r = rect.current;
    if (!r || !r.w) return measure();
    const nx = Math.max(-1, Math.min(1, ((e.nativeEvent.pageX - r.x) / r.w) * 2 - 1));
    const ny = Math.max(-1, Math.min(1, ((e.nativeEvent.pageY - r.y) / r.h) * 2 - 1));
    spring(px, nx * -12, { stiffness: 40, damping: 18 }).start();
    spring(py, ny * -7, { stiffness: 40, damping: 18 }).start();
  };

  const bgTransform = [
    { translateX: Animated.add(px, drift.interpolate({ inputRange: [0, 1], outputRange: [-14, 14] })) },
    { translateY: Animated.add(py, drift.interpolate({ inputRange: [0, 1], outputRange: [6, -6] })) },
    { scale: drift.interpolate({ inputRange: [0, 1], outputRange: [1.08, 1.12] }) },
  ];

  return (
    <View
      ref={rootRef}
      style={styles.root}
      onPointerEnter={measure}
      onPointerMove={active ? onPointerMove : undefined}
      onKeyDown={(e) => {
        if (e.key === 'Enter' || e.key === ' ') pulse('A');
        else if (e.key === 'Escape') pulse('B');
      }}
      onGamepadButton={(e) => {
        if (!e.pressed) return;
        if (e.buttonIndex === 0) pulse('A');
        else if (e.buttonIndex === 1) pulse('B');
        else if (e.buttonIndex === 3) {
          pulse('Y');
          setToast({ text: 'Opening patch notes 4.2', at: Date.now() });
          e.preventDefault();
        }
      }}
    >
      <Animated.View style={[StyleSheet.absoluteFill, { transform: bgTransform }]}>
        <Image source={require('../../../examples/assets/bg-menu.jpg')} style={{ width: 1280, height: 720 }} resizeMode="cover" />
      </Animated.View>
      <View pointerEvents="none" style={styles.scrim} />
      {EMBERS.map((e, i) => (
        <Ember key={i} e={e} active={active} />
      ))}

      <Animated.View style={[styles.topBar, { opacity: intro, transform: [{ translateY: intro.interpolate({ inputRange: [0, 1], outputRange: [-20, 0] }) }] }]}>
        <Pressable onPress={() => setToast({ text: 'Opening profile · Valkyrie_07', at: Date.now() })} focusable={false} group style={{ cursor: 'pointer' }}>
          <View style={styles.profile} groupHoveredStyle={styles.profileHot}>
            <Image source={require('../../../examples/assets/crest-wolf.png')} style={styles.crest} />
            <View>
              <Text style={styles.profileName}>Valkyrie_07</Text>
              <Text style={styles.profileLevel}>Level 42 · Vanguard</Text>
            </View>
            <View style={styles.divider} />
            <View style={styles.currency}>
              <View style={[styles.coin, { experimental_backgroundImage: 'radial-gradient(circle at 35% 35%, #fff2c2 0%, #f2c35b 45%, #a8741f 100%)' }]} />
              <Text style={styles.currencyText}>12,480</Text>
            </View>
            <View style={styles.currency}>
              <View style={[styles.coin, { borderRadius: 3, transform: [{ rotate: '45deg' }], experimental_backgroundImage: 'linear-gradient(135deg, #d6b8ff 0%, #8a4dff 100%)' }]} />
              <Text style={styles.currencyText}>860</Text>
            </View>
          </View>
        </Pressable>
      </Animated.View>

      <View style={styles.left}>
        <Animated.View style={{ opacity: intro, transform: [{ translateY: intro.interpolate({ inputRange: [0, 1], outputRange: [16, 0] }) }] }}>
          <Text style={styles.season}>Season 4 — Emberfall</Text>
          <View>
            <Text style={styles.title}>AETHERFALL</Text>
            <Animated.Text pointerEvents="none" style={[styles.title, styles.titleGlow, { opacity: glow }]}>
              AETHERFALL
            </Animated.Text>
          </View>
          <View style={styles.titleRule}>
            <Animated.View style={[styles.ruleLine, { transform: [{ scaleX: intro }], transformOrigin: 'right' }]} />
            <Animated.View
              style={[
                styles.ruleDiamond,
                { transform: [{ rotate: '45deg' }, { scale: glow.interpolate({ inputRange: [0, 1], outputRange: [1, 1.35] }) }] },
              ]}
            />
            <Animated.View
              style={[
                styles.ruleLine,
                { experimental_backgroundImage: 'linear-gradient(90deg, rgba(242,195,91,0.9), rgba(242,195,91,0))', transform: [{ scaleX: intro }], transformOrigin: 'left' },
              ]}
            />
          </View>
          <Text style={styles.subtitle}>Chronicles of the Shattered Sky</Text>
        </Animated.View>

        <View style={styles.menu}>
          <Animated.View pointerEvents="none" style={[styles.focusBar, { opacity: items[sel], transform: [{ translateY: barY }] }]} />
          {ITEMS.map((it, i) => (
            <MenuButton
              key={it.id}
              item={it}
              index={i}
              selected={sel === i}
              enter={items[i]}
              onSelect={select}
              onActivate={activate}
              autoFocus={autoFocus && i === 0}
            />
          ))}
        </View>
      </View>

      <NewsCard active={active && !confirm} enter={news} />

      <Animated.View style={[styles.bottomBar, { opacity: intro }]}>
        <KeyHint glyph="A" label="Select" color="#5fe08a" flash={flash.A} />
        <KeyHint glyph="B" label="Back" color="#ff6b81" flash={flash.B} />
        <KeyHint glyph="Y" label="Patch Notes" color="#ffd36b" flash={flash.Y} />
        <View style={{ flex: 1 }} />
        <Animated.View style={[styles.pingDot, { backgroundColor: pingColor }]} />
        <Animated.Text style={styles.version} text={pingText} />
      </Animated.View>

      <Toast toast={toast} />
      {confirm ? (
        <QuitDialog
          onCancel={() => setConfirm(false)}
          onQuit={() => {
            setConfirm(false);
            setToast({ text: 'Nice try. The adventure continues.', at: Date.now() });
          }}
        />
      ) : null}
    </View>
  );
}

const styles = StyleSheet.create({
  root: { width: 1280, height: 720, overflow: 'hidden', backgroundColor: colors.bg0 },
  scrim: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage:
      'linear-gradient(90deg, rgba(5,7,16,0.92) 0%, rgba(5,7,16,0.7) 38%, rgba(5,7,16,0) 70%), linear-gradient(180deg, rgba(5,7,16,0) 70%, rgba(5,7,16,0.85) 100%)',
  },
  topBar: { position: 'absolute', top: 28, right: 32, flexDirection: 'row' },
  profile: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    paddingVertical: 8,
    paddingLeft: 8,
    paddingRight: 18,
    borderRadius: 30,
    backgroundColor: 'rgba(10, 14, 28, 0.72)',
    borderWidth: 1,
    borderColor: colors.line,
    boxShadow: '0 8px 24px rgba(0,0,0,0.45)',
  },
  profileHot: { borderColor: 'rgba(242,195,91,0.6)', boxShadow: '0 8px 24px rgba(0,0,0,0.45), 0 0 18px rgba(242,195,91,0.3)' },
  crest: { width: 40, height: 40, borderRadius: 20, borderWidth: 2, borderColor: colors.gold },
  profileName: { fontFamily: font, fontWeight: '600', fontSize: 15, color: colors.text },
  profileLevel: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textDim, marginTop: 1 },
  divider: { width: 1, height: 28, backgroundColor: colors.line, marginHorizontal: 4 },
  currency: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  coin: { width: 16, height: 16, borderRadius: 8, boxShadow: '0 0 8px rgba(242,195,91,0.5)' },
  currencyText: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.text },

  left: { position: 'absolute', left: 88, top: 64, width: 600 },
  season: { ...type.label, color: colors.gold, letterSpacing: 3 },
  title: {
    fontFamily: font,
    fontWeight: '700',
    fontSize: 64,
    letterSpacing: 7,
    color: '#fff6dc',
    marginTop: 10,
    textShadowColor: 'rgba(242, 195, 91, 0.65)',
    textShadowOffset: { width: 0, height: 0 },
    textShadowRadius: 24,
  },
  titleGlow: { position: 'absolute', left: 0, top: 0, textShadowColor: 'rgba(255, 210, 120, 0.9)', textShadowRadius: 40, color: 'rgba(255,246,220,0.35)' },
  titleRule: { flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 6, width: 380 },
  ruleLine: { flex: 1, height: 1, backgroundColor: 'rgba(242,195,91,0.9)' },
  ruleDiamond: { width: 9, height: 9, backgroundColor: colors.gold, boxShadow: '0 0 10px rgba(242,195,91,0.9)' },
  subtitle: { fontFamily: font, fontStyle: 'italic', fontSize: 17, color: colors.textDim, marginTop: 12, letterSpacing: 0.5 },

  menu: { marginTop: 32, gap: GAP, width: 360 },
  button: {
    height: ROW,
    flexDirection: 'row',
    alignItems: 'center',
    paddingHorizontal: 22,
    borderRadius: 8,
    backgroundColor: 'rgba(14, 20, 38, 0.55)',
    borderWidth: 1,
    borderColor: 'rgba(148, 170, 255, 0.10)',
  },
  buttonSelectedShape: { height: ROW_SEL, borderColor: 'rgba(242, 195, 91, 0.85)' },
  buttonGlow: {
    borderRadius: 7,
    experimental_backgroundImage: 'linear-gradient(90deg, rgba(242,195,91,0.32) 0%, rgba(242,195,91,0.06) 100%)',
    boxShadow: '0 0 0 1px rgba(242,195,91,0.25), 0 0 28px rgba(242,195,91,0.35), inset 0 0 18px rgba(242,195,91,0.18)',
  },
  focusBar: {
    position: 'absolute',
    left: 9,
    top: 0,
    width: 4,
    height: ROW_SEL - 20,
    zIndex: 5,
    borderTopRightRadius: 3,
    borderBottomRightRadius: 3,
    backgroundColor: colors.goldLight,
    boxShadow: '0 0 12px rgba(255,226,154,0.9)',
  },
  buttonText: { fontFamily: font, fontWeight: '600', fontSize: 17, letterSpacing: 1.5, textTransform: 'uppercase', color: '#c9d2ea' },
  buttonTextFocused: { color: '#fff6dc', fontWeight: '700' },
  buttonHint: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.gold, marginTop: 3, letterSpacing: 0.4 },
  chevron: { fontFamily: font, fontWeight: '400', fontSize: 30, color: colors.goldLight, marginTop: -4, marginLeft: 10 },
  badge: { paddingHorizontal: 8, paddingVertical: 3, borderRadius: 10, backgroundColor: colors.red, boxShadow: '0 0 10px rgba(255,77,106,0.6)' },
  badgeText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: '#fff' },

  newsWrap: { position: 'absolute', right: 32, bottom: 84, width: 340 },
  news: {
    borderRadius: 14,
    overflow: 'hidden',
    backgroundColor: colors.panelSolid,
    borderWidth: 1,
    borderColor: colors.line,
    boxShadow: '0 20px 50px rgba(0,0,0,0.6)',
  },
  newsFocused: { borderColor: 'rgba(242,195,91,0.85)', boxShadow: '0 20px 50px rgba(0,0,0,0.6), 0 0 0 2px rgba(242,195,91,0.35), 0 0 24px rgba(242,195,91,0.3)' },
  newsArtClip: { height: 120, overflow: 'hidden' },
  newsArt: { width: '100%', height: 120 },
  newsArtFade: { ...StyleSheet.absoluteFillObject, experimental_backgroundImage: 'linear-gradient(180deg, rgba(18,26,46,0) 55%, rgba(18,26,46,0.8) 100%)' },
  newsTag: { position: 'absolute', top: 12, left: 12, paddingHorizontal: 9, paddingVertical: 4, borderRadius: 6 },
  newsTagText: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: '#2a1300' },
  newsCount: { position: 'absolute', top: 12, right: 12, paddingHorizontal: 8, paddingVertical: 3, borderRadius: 6, backgroundColor: 'rgba(5,7,16,0.6)' },
  newsCountText: { fontFamily: font, fontWeight: '600', fontSize: 11, color: colors.textDim },
  newsBody: { padding: 16, paddingTop: 14 },
  newsKicker: { ...type.label, fontSize: 11 },
  newsTitle: { fontFamily: font, fontWeight: '700', fontSize: 19, color: colors.text, marginTop: 6 },
  newsText: { fontFamily: font, fontSize: 13, lineHeight: 19, color: colors.textDim, marginTop: 6 },
  pips: { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 4 },
  pip: { width: 8, height: 4, borderRadius: 2, backgroundColor: colors.lineStrong, overflow: 'hidden' },
  pipActive: { width: 22 },
  pipFill: { ...StyleSheet.absoluteFillObject, transformOrigin: 'left' },
  newsMore: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1, textTransform: 'uppercase', color: colors.textMute },

  bottomBar: {
    position: 'absolute',
    left: 0,
    right: 0,
    bottom: 0,
    height: 56,
    paddingHorizontal: 88,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 28,
    borderTopWidth: 1,
    borderTopColor: 'rgba(148,170,255,0.10)',
    backgroundColor: 'rgba(5,7,16,0.6)',
  },
  keyHint: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  keyGlyph: { width: 24, height: 24, borderRadius: 12, borderWidth: 2, alignItems: 'center', justifyContent: 'center' },
  keyGlyphText: { fontFamily: font, fontWeight: '700', fontSize: 12 },
  keyLabel: { fontFamily: font, fontWeight: '500', fontSize: 14, color: colors.textDim },
  pingDot: { width: 6, height: 6, borderRadius: 3, marginRight: -20 },
  version: { fontFamily: font, fontSize: 12, color: colors.textMute, letterSpacing: 0.5 },

  toast: {
    position: 'absolute',
    top: 28,
    left: 440,
    width: 400,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 14,
    paddingVertical: 12,
    paddingHorizontal: 16,
    borderRadius: 12,
    backgroundColor: 'rgba(12,16,30,0.94)',
    borderWidth: 1,
    borderColor: 'rgba(242,195,91,0.5)',
    boxShadow: '0 16px 40px rgba(0,0,0,0.6), 0 0 24px rgba(242,195,91,0.2)',
  },
  toastSpinner: { width: 26, height: 26, alignItems: 'center', justifyContent: 'center' },
  toastDiamond: { width: 14, height: 14, borderWidth: 2, borderColor: colors.gold, boxShadow: '0 0 10px rgba(242,195,91,0.8)' },
  toastText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
  toastTrack: { height: 3, borderRadius: 2, marginTop: 8, backgroundColor: 'rgba(255,255,255,0.08)', overflow: 'hidden' },
  toastFill: { ...StyleSheet.absoluteFillObject, transformOrigin: 'left', experimental_backgroundImage: 'linear-gradient(90deg, #a8741f, #ffe29a)' },

  dialogScrim: { ...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(3,5,12,0.72)', alignItems: 'center', justifyContent: 'center' },
  dialog: {
    width: 440,
    padding: 28,
    borderRadius: 16,
    backgroundColor: 'rgba(14,20,36,0.98)',
    borderWidth: 1,
    borderColor: 'rgba(242,195,91,0.45)',
    borderTopWidth: 3,
    borderTopColor: colors.gold,
    boxShadow: '0 30px 80px rgba(0,0,0,0.8), 0 0 40px rgba(242,195,91,0.15)',
  },
  dialogKicker: { ...type.label, color: colors.gold, letterSpacing: 2.4 },
  dialogTitle: { fontFamily: font, fontWeight: '700', fontSize: 26, color: colors.text, marginTop: 6, letterSpacing: 1 },
  dialogText: { fontFamily: font, fontSize: 14, lineHeight: 21, color: colors.textDim, marginTop: 10 },
  dialogRow: { flexDirection: 'row', gap: 12, marginTop: 24 },
  dialogBtn: {
    height: 48,
    borderRadius: 10,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    gap: 10,
    backgroundColor: 'rgba(255,255,255,0.05)',
    borderWidth: 1,
    borderColor: colors.lineStrong,
  },
  pressed96: { transform: [{ scale: 0.96 }] },
  dialogBtnHot: { borderColor: colors.goldLight, backgroundColor: 'rgba(242,195,91,0.16)', boxShadow: '0 0 20px rgba(242,195,91,0.35)' },
  dialogBtnDanger: { borderColor: 'rgba(255,77,106,0.4)' },
  dialogBtnDangerHot: { borderColor: colors.red, backgroundColor: 'rgba(255,77,106,0.18)', boxShadow: '0 0 20px rgba(255,77,106,0.4)' },
  dialogBtnText: { fontFamily: font, fontWeight: '700', fontSize: 15, letterSpacing: 1.5, textTransform: 'uppercase', color: colors.text },
});
