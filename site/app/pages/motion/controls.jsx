// Small controls shared by the motion lab demos: a slider (pointer drag,
// keyboard arrows, gamepad D-pad), a segmented picker and the demo card shell.
import React, { useMemo, useRef, useState } from 'react';
import { Animated, PanResponder, Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site, text } from '../../theme.js';
import { Card, Pill, Reveal, useLayout } from '../../components/ui.jsx';
import { CodeBlock } from '../../components/CodeBlock.jsx';

/** Focus ring only for keyboard / gamepad focus (like :focus-visible). */
export function useFocusVisible() {
  const [vis, setVis] = useState(false);
  return [vis, { onFocus: (e) => setVis(!!(e && e.nativeEvent && e.nativeEvent.focusVisible)), onBlur: () => setVis(false) }];
}

const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

/**
 * Horizontal slider. Drag or click the track; when focused, ←/→ (or the
 * D-pad) step it, Shift steps ×10, Home/End jump to the ends.
 */
export function Slider({ label, value, min, max, step = 1, onChange, color = colors.gold, format = (v) => String(v), style }) {
  const [w, setW] = useState(0);
  const [focused, setFocused] = useState(false);
  const [dragging, setDragging] = useState(false);
  const live = useRef({ w, min, max, step, onChange, start: 0 });
  live.current.w = w;
  live.current.min = min;
  live.current.max = max;
  live.current.step = step;
  live.current.onChange = onChange;
  const fromX = (x) => {
    const L = live.current;
    if (!L.w) return L.min;
    const raw = L.min + clamp(x / L.w, 0, 1) * (L.max - L.min);
    const v = Math.round(raw / L.step) * L.step;
    return +clamp(v, L.min, L.max).toFixed(4);
  };
  const pan = useMemo(
    () =>
      PanResponder.create({
        onStartShouldSetPanResponder: () => true,
        onMoveShouldSetPanResponder: () => true,
        onPanResponderTerminationRequest: () => false,
        onPanResponderGrant: (e) => {
          live.current.start = e.nativeEvent.locationX;
          setDragging(true);
          live.current.onChange(fromX(live.current.start));
        },
        onPanResponderMove: (e, g) => live.current.onChange(fromX(live.current.start + g.dx)),
        onPanResponderRelease: () => setDragging(false),
        onPanResponderTerminate: () => setDragging(false),
      }),
    [],
  );
  const nudge = (dir, big) => onChange(+clamp(value + dir * step * (big ? 10 : 1), min, max).toFixed(4));
  const p = (value - min) / (max - min || 1);
  return (
    <View style={[{ gap: 8 }, style]}>
      <View style={styles.sliderHead}>
        <Text style={styles.sliderLabel}>{label}</Text>
        <Text style={[styles.sliderValue, { color }]}>{format(value)}</Text>
      </View>
      <View
        focusable
        onFocus={(e) => setFocused(!!e.nativeEvent.focusVisible)}
        onBlur={() => setFocused(false)}
        onKeyDown={(e) => {
          const k = e.nativeEvent.key;
          if (k === 'ArrowLeft' || k === 'ArrowDown') nudge(-1, e.nativeEvent.shiftKey);
          else if (k === 'ArrowRight' || k === 'ArrowUp') nudge(1, e.nativeEvent.shiftKey);
          else if (k === 'Home') onChange(min);
          else if (k === 'End') onChange(max);
          else return;
          e.preventDefault();
        }}
        onGamepadButton={(e) => {
          const b = e.nativeEvent.buttonIndex;
          if (!e.nativeEvent.pressed || (b !== 14 && b !== 15)) return;
          nudge(b === 14 ? -1 : 1, false);
          e.preventDefault();
        }}
        onLayout={(e) => setW(e.nativeEvent.layout.width)}
        style={[styles.track, { cursor: 'pointer' }]}
        {...pan.panHandlers}
      >
        <View pointerEvents="none" style={styles.rail}>
          <View style={[styles.fill, { width: `${p * 100}%`, backgroundColor: color }]} />
        </View>
        <View
          pointerEvents="none"
          style={[
            styles.thumb,
            { left: p * w - 9, borderColor: color },
            (focused || dragging) && { boxShadow: `0 0 0 5px ${color}33, 0 2px 8px rgba(0,0,0,0.5)` },
            dragging && { transform: [{ scale: 1.15 }] },
          ]}
        />
      </View>
    </View>
  );
}

/** Row of mutually exclusive options. */
export function Segmented({ options, value, onChange, color = colors.cyan, small }) {
  return (
    <View style={styles.seg}>
      {options.map((o) => {
        const id = typeof o === 'string' ? o : o.id;
        const label = typeof o === 'string' ? o : o.label;
        return <SegItem key={id} label={label} on={id === value} onPress={() => onChange(id)} color={color} small={small} />;
      })}
    </View>
  );
}

function SegItem({ label, on, onPress, color, small }) {
  const [vis, fv] = useFocusVisible();
  return (
          <Pressable onPress={onPress} {...fv} style={{ cursor: 'pointer' }}>
            {({ hovered, pressed }) => {
              const focused = vis;
              return (
              <View
                style={[
                  styles.segItem,
                  small && { height: 30, paddingHorizontal: 10 },
                  hovered && !on && { backgroundColor: 'rgba(255,255,255,0.06)' },
                  on && { backgroundColor: color + '22', borderColor: color + '88' },
                  focused && { boxShadow: `0 0 0 2px ${colors.cyan}` },
                  pressed && { transform: [{ scale: 0.96 }] },
                ]}
              >
                <Text style={[styles.segText, small && { fontSize: 12 }, on && { color }]}>{label}</Text>
              </View>
              );
            }}
          </Pressable>
  );
}

/** Small press-springy button used inside demos. */
export function DemoButton({ label, icon, onPress, color = colors.gold, primary }) {
  const s = useRef(new Animated.Value(1)).current;
  const to = (v) => Animated.spring(s, { toValue: v, stiffness: 500, damping: 22, useNativeDriver: true }).start();
  const [focused, fv] = useFocusVisible();
  return (
    <Pressable {...fv} onPress={onPress} onPressIn={() => to(0.92)} onPressOut={() => to(1)} onHoverIn={() => to(1.05)} onHoverOut={() => to(1)} style={{ cursor: 'pointer' }}>
      {({ hovered }) => (
        <Animated.View
          style={[
            styles.dbtn,
            primary ? { backgroundColor: color, borderColor: color } : { borderColor: color + '66', backgroundColor: color + (hovered ? '26' : '12') },
            focused && { boxShadow: `0 0 0 2px ${site.bg}, 0 0 0 4px ${colors.cyan}` },
            { transform: [{ scale: s }] },
          ]}
        >
          {icon ? <Text style={[styles.dbtnText, { color: primary ? '#160f04' : color }]}>{icon}</Text> : null}
          <Text style={[styles.dbtnText, { color: primary ? '#160f04' : color }]}>{label}</Text>
        </Animated.View>
      )}
    </Pressable>
  );
}

/**
 * One demo: title + blurb + tags, the live demo, and its code. Wide screens put
 * the code beside the demo, narrower ones below it.
 */
export function DemoCard({ index, title, blurb, tags = [], code, codeTitle, children, demoMin = 1.35 }) {
  const { width } = useLayout();
  const wide = width >= 1180;
  return (
    <Reveal>
      <Card style={[styles.demoCard, width < 640 && { padding: 16 }]}>
        <View style={styles.demoHead}>
          <Text style={styles.demoIndex}>{String(index).padStart(2, '0')}</Text>
          <View style={{ flex: 1, gap: 6, minWidth: 220 }}>
            <Text style={text.h2}>{title}</Text>
            <Text style={[text.body, { maxWidth: 760 }]}>{blurb}</Text>
          </View>
          <View style={styles.tags}>
            {tags.map(([t, c]) => (
              <Pill key={t} color={c || colors.cyan}>
                {t}
              </Pill>
            ))}
          </View>
        </View>
        <View style={[styles.demoBody, !wide && { flexDirection: 'column', alignItems: 'stretch' }]}>
          <View style={[styles.stageArea, wide ? { flex: demoMin } : null]}>{children}</View>
          <View style={wide ? { flex: 1, minWidth: 0 } : null}>
            <CodeBlock title={codeTitle} code={code} />
          </View>
        </View>
      </Card>
    </Reveal>
  );
}

const styles = StyleSheet.create({
  sliderHead: { flexDirection: 'row', alignItems: 'baseline', justifyContent: 'space-between' },
  sliderLabel: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textDim },
  sliderValue: { fontFamily: mono, fontWeight: '700', fontSize: 13 },
  track: { height: 28, justifyContent: 'center', borderRadius: 8 },
  rail: { height: 6, borderRadius: 3, backgroundColor: 'rgba(148,170,255,0.16)', overflow: 'hidden' },
  fill: { height: 6, borderRadius: 3 },
  thumb: { position: 'absolute', top: 5, width: 18, height: 18, borderRadius: 9, backgroundColor: '#f4f6ff', borderWidth: 3, boxShadow: '0 2px 8px rgba(0,0,0,0.5)' },
  seg: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  segItem: { height: 34, paddingHorizontal: 14, borderRadius: 9, borderWidth: 1, borderColor: site.stroke, justifyContent: 'center', backgroundColor: 'rgba(255,255,255,0.02)' },
  segText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textDim },
  dbtn: { height: 36, paddingHorizontal: 14, borderRadius: 10, borderWidth: 1, flexDirection: 'row', alignItems: 'center', gap: 8 },
  dbtnText: { fontFamily: font, fontWeight: '700', fontSize: 13 },
  demoCard: { padding: 28, gap: 22 },
  demoHead: { flexDirection: 'row', flexWrap: 'wrap', gap: 16, alignItems: 'flex-start' },
  demoIndex: { fontFamily: mono, fontWeight: '700', fontSize: 14, color: colors.gold, marginTop: 7 },
  tags: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, maxWidth: 360 },
  demoBody: { flexDirection: 'row', gap: 22, alignItems: 'flex-start' },
  stageArea: {
    borderRadius: 14,
    borderWidth: 1,
    borderColor: site.stroke,
    backgroundColor: 'rgba(5,8,18,0.7)',
    padding: 18,
    gap: 16,
    overflow: 'hidden',
  },
});
