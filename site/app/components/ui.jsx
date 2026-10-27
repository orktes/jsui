// Shared building blocks for the site pages.
import React, { useEffect, useRef } from 'react';
import { Animated, Easing, Linking, Pressable, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import { bp, colors, font, site, text } from '../theme.js';
import { useRouter } from '../router.js';

export const GITHUB = 'https://github.com/orktes/jsui';

/** Layout info for the current canvas size. */
export function useLayout() {
  const { width, height } = useWindowDimensions();
  return {
    width,
    height,
    phone: width < bp.phone,
    tablet: width < bp.tablet,
    gutter: width < bp.phone ? 18 : width < bp.tablet ? 28 : 40,
    content: Math.min(width, 1240),
  };
}

/** Centred page column with responsive gutters. */
export function Column({ children, style, max = 1240, onLayout }) {
  const { gutter } = useLayout();
  return (
    <View onLayout={onLayout} style={[{ width: '100%', maxWidth: max + gutter * 2, alignSelf: 'center', paddingHorizontal: gutter }, style]}>
      {children}
    </View>
  );
}

export function Eyebrow({ children, color = colors.gold, style }) {
  return <Text style={[text.eyebrow, { color }, style]}>{children}</Text>;
}

/** Section heading block: eyebrow, title, optional lead paragraph. */
export function SectionHead({ eyebrow, title, lead, align = 'left', style }) {
  const { phone } = useLayout();
  return (
    <View style={[{ gap: 12, maxWidth: 760, alignSelf: align === 'center' ? 'center' : 'flex-start' }, style]}>
      {eyebrow ? <Eyebrow style={{ textAlign: align }}>{eyebrow}</Eyebrow> : null}
      <Text style={[phone ? text.h2 : text.h1, { textAlign: align }]}>{title}</Text>
      {lead ? <Text style={[text.lead, phone && { fontSize: 16, lineHeight: 25 }, { textAlign: align }]}>{lead}</Text> : null}
    </View>
  );
}

export function Card({ children, style, glow }) {
  return <View style={[styles.card, glow && { borderColor: glow, boxShadow: `0 0 0 1px ${glow}, 0 24px 60px rgba(0,0,0,0.5)` }, style]}>{children}</View>;
}

/**
 * Button with hover / focus / press feedback (springs on the native driver).
 * kind: 'primary' | 'ghost' | 'quiet'. `to` navigates inside the site, `href` opens a URL.
 */
export function Button({ label, kind = 'primary', to, href, onPress, icon, small, style, autoFocus, self = 'flex-start' }) {
  const { go } = useRouter();
  const scale = useRef(new Animated.Value(1)).current;
  const spring = (v) => Animated.spring(scale, { toValue: v, useNativeDriver: true, speed: 40, bounciness: 8 }).start();
  const press = () => {
    if (onPress) onPress();
    if (to) go(to);
    if (href) Linking.openURL(href);
  };
  return (
    <Pressable
      onPress={press}
      onHoverIn={() => spring(1.04)}
      onHoverOut={() => spring(1)}
      onPressIn={() => spring(0.96)}
      onPressOut={() => spring(1)}
      autoFocus={autoFocus}
      style={{ alignSelf: self, cursor: 'pointer' }}
    >
      {({ hovered, focused, pressed }) => (
        <Animated.View
          style={[
            styles.btn,
            small && styles.btnSmall,
            kind === 'primary' && styles.btnPrimary,
            kind === 'ghost' && styles.btnGhost,
            kind === 'quiet' && styles.btnQuiet,
            kind === 'primary' && (hovered || focused) && styles.btnPrimaryHot,
            kind !== 'primary' && (hovered || focused) && styles.btnGhostHot,
            focused && styles.focusRing,
            { transform: [{ scale }] },
            style,
          ]}
        >
          {icon ? <Text style={[styles.btnText, small && styles.btnTextSmall, kind === 'primary' && { color: '#1a1206' }]}>{icon}</Text> : null}
          <Text style={[styles.btnText, small && styles.btnTextSmall, kind === 'primary' && { color: '#1a1206' }, pressed && { opacity: 0.85 }]}>
            {label}
          </Text>
        </Animated.View>
      )}
    </Pressable>
  );
}

export function Pill({ children, color = colors.cyan, style }) {
  return (
    <View style={[styles.pill, { borderColor: color + '55', backgroundColor: color + '14' }, style]}>
      <Text style={[styles.pillText, { color }]}>{children}</Text>
    </View>
  );
}

/**
 * Renders `children` laid out at a fixed design size (default 1280×720) and
 * scales it to fit `width`. Hit testing follows the transform, so the scaled
 * screen stays fully interactive.
 */
export function Stage({ width, design = { width: 1280, height: 720 }, children, style, radius = 18 }) {
  const s = width / design.width;
  const h = Math.round(design.height * s);
  return (
    <View style={[styles.stage, { width, height: h, borderRadius: radius }, style]}>
      <View
        style={{
          position: 'absolute',
          width: design.width,
          height: design.height,
          left: (width - design.width) / 2,
          top: (h - design.height) / 2,
          transform: [{ scale: s }],
        }}
      >
        {children}
      </View>
    </View>
  );
}

/** Fades + lifts its children in when mounted (staggered by `delay`). */
export function Reveal({ children, delay = 0, style, distance = 18 }) {
  const t = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    Animated.timing(t, { toValue: 1, duration: 620, delay, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, []);
  return (
    <Animated.View
      style={[
        style,
        { opacity: t, transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [distance, 0] }) }] },
      ]}
    >
      {children}
    </Animated.View>
  );
}

export function Divider({ style }) {
  return <View style={[{ height: 1, backgroundColor: site.stroke }, style]} />;
}

/** Keyboard / gamepad glyph chip, e.g. <Key>Tab</Key>. */
export function Key({ children, color = colors.textDim }) {
  return (
    <View style={[styles.key, { borderColor: color + '66' }]}>
      <Text style={[styles.keyText, { color }]}>{children}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  card: {
    backgroundColor: site.card,
    borderWidth: 1,
    borderColor: site.stroke,
    borderRadius: 18,
    padding: 24,
    boxShadow: '0 24px 60px rgba(0, 0, 0, 0.45), inset 0 1px 0 rgba(255, 255, 255, 0.05)',
  },
  btn: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingHorizontal: 22,
    height: 48,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: 'transparent',
  },
  btnSmall: { height: 36, paddingHorizontal: 14, borderRadius: 10 },
  btnPrimary: {
    experimental_backgroundImage: 'linear-gradient(180deg, #ffe29a 0%, #f2c35b 55%, #d99a32 100%)',
    boxShadow: '0 10px 30px rgba(242, 195, 91, 0.25), inset 0 1px 0 rgba(255,255,255,0.6)',
  },
  btnPrimaryHot: { boxShadow: '0 14px 40px rgba(242, 195, 91, 0.45), inset 0 1px 0 rgba(255,255,255,0.7)' },
  btnGhost: { borderColor: site.strokeStrong, backgroundColor: 'rgba(255,255,255,0.03)' },
  btnQuiet: { backgroundColor: 'transparent' },
  btnGhostHot: { borderColor: 'rgba(79, 209, 255, 0.6)', backgroundColor: 'rgba(79, 209, 255, 0.08)' },
  focusRing: { boxShadow: '0 0 0 2px #05070f, 0 0 0 4px rgba(79, 209, 255, 0.9)' },
  btnText: { fontFamily: font, fontWeight: '600', fontSize: 15, color: colors.text, letterSpacing: 0.2 },
  btnTextSmall: { fontSize: 13 },
  pill: { alignSelf: 'flex-start', borderWidth: 1, borderRadius: 999, paddingHorizontal: 10, paddingVertical: 4 },
  pillText: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 0.4 },
  stage: {
    overflow: 'hidden',
    backgroundColor: '#000',
    borderWidth: 1,
    borderColor: site.strokeStrong,
    boxShadow: '0 40px 120px rgba(0, 0, 0, 0.7), 0 0 0 1px rgba(0,0,0,0.6)',
  },
  key: { minWidth: 26, height: 26, paddingHorizontal: 7, borderRadius: 7, borderWidth: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(255,255,255,0.04)' },
  keyText: { fontFamily: font, fontWeight: '700', fontSize: 12 },
});
