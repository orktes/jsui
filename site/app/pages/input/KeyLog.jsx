// Keyboard event log: a focusable panel with onKeyDown / onKeyUp that shows
// every event's key, code and modifiers, plus a keycap cluster that lights
// up for held keys. While focused it keeps arrows / Space for itself
// (preventDefault); Tab leaves, Escape blurs.
import React, { memo, useEffect, useRef, useState } from 'react';
import { Animated, Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';

const springTo = (v, to) => Animated.spring(v, { toValue: to, stiffness: 500, damping: 24, mass: 0.6, useNativeDriver: true });

const Cap = memo(function Cap({ label, on, w = 34 }) {
  const v = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    springTo(v, on ? 1 : 0).start();
  }, [on]);
  return (
    <Animated.View
      style={[
        styles.cap,
        { width: w, transform: [{ translateY: v.interpolate({ inputRange: [0, 1], outputRange: [0, 2] }) }] },
      ]}
    >
      <Animated.View style={[StyleSheet.absoluteFill, styles.capLit, { opacity: v }]} />
      <Text style={[styles.capText, on && { color: '#0b1020' }]}>{label}</Text>
    </Animated.View>
  );
});

const pretty = (k) => (k === ' ' ? 'Space' : k);
let nextId = 0;

export function KeyLog({ compact }) {
  const ref = useRef(null);
  const [focused, setFocused] = useState(false);
  const [held, setHeld] = useState({});
  const [log, setLog] = useState([]);
  const ring = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    springTo(ring, focused ? 1 : 0).start();
  }, [focused]);

  const push = (type, e) => {
    const item = {
      id: ++nextId,
      type,
      key: e.key,
      code: e.code || '',
      mods: [e.ctrlKey && 'Ctrl', e.altKey && 'Alt', e.shiftKey && 'Shift', e.metaKey && 'Meta'].filter(Boolean),
      repeat: !!e.repeat,
    };
    setLog((l) => [item, ...l].slice(0, 9));
  };
  const onKeyDown = (e) => {
    if (e.key === 'Tab') return; // let focus move on
    if (e.key === 'Escape') {
      ref.current && ref.current.blur();
      return;
    }
    e.preventDefault();
    push('down', e);
    setHeld((h) => (h[e.code || e.key] ? h : { ...h, [e.code || e.key]: true }));
  };
  const onKeyUp = (e) => {
    push('up', e);
    setHeld((h) => {
      const n = { ...h };
      delete n[e.code || e.key];
      return n;
    });
  };
  const has = (...codes) => codes.some((c) => held[c]);

  return (
    <Pressable
      ref={ref}
      onKeyDown={onKeyDown}
      onKeyUp={onKeyUp}
      onFocus={() => setFocused(true)}
      onBlur={() => {
        setFocused(false);
        setHeld({});
      }}
      style={styles.panel}
    >
      <Animated.View pointerEvents="none" style={[styles.ring, { opacity: ring }]} />
      <View style={styles.head}>
        <View style={[styles.dot, { backgroundColor: focused ? colors.green : colors.textMute }]} />
        <Text style={styles.headText}>{focused ? 'Listening: type anything' : 'Click here (or Tab to it) to capture keys'}</Text>
      </View>

      <View style={[styles.keys, compact && { transform: [{ scale: 0.86 }] }]}>
        <View style={styles.keyRow}>
          <Cap label="Esc" on={has('Escape')} w={44} />
          <Cap label="W" on={has('KeyW')} />
          <View style={{ flex: 1 }} />
          <Cap label="↑" on={has('ArrowUp')} />
        </View>
        <View style={styles.keyRow}>
          <Cap label="Tab" on={has('Tab')} w={44} />
          <Cap label="A" on={has('KeyA')} />
          <Cap label="S" on={has('KeyS')} />
          <Cap label="D" on={has('KeyD')} />
          <View style={{ flex: 1 }} />
          <Cap label="←" on={has('ArrowLeft')} />
          <Cap label="↓" on={has('ArrowDown')} />
          <Cap label="→" on={has('ArrowRight')} />
        </View>
        <View style={styles.keyRow}>
          <Cap label="Shift" on={has('ShiftLeft', 'ShiftRight', 'Shift')} w={56} />
          <Cap label="Ctrl" on={has('ControlLeft', 'ControlRight', 'Control')} w={44} />
          <Cap label="Alt" on={has('AltLeft', 'AltRight', 'Alt')} w={40} />
          <Cap label="Space" on={has('Space', ' ')} w={compact ? 76 : 110} />
          <Cap label="Enter" on={has('Enter', 'NumpadEnter')} w={56} />
        </View>
      </View>

      <View style={styles.table}>
        <View style={[styles.tr, styles.th]}>
          <Text style={[styles.tdType, styles.thText]}>EVENT</Text>
          <Text style={[styles.tdKey, styles.thText]}>KEY</Text>
          <Text style={[styles.tdCode, styles.thText]}>CODE</Text>
          {!compact ? <Text style={[styles.tdMods, styles.thText]}>MODIFIERS</Text> : null}
        </View>
        {log.length === 0 ? (
          <Text style={styles.empty}>onKeyDown / onKeyUp events will stream in here.</Text>
        ) : (
          log.map((r, i) => (
            <View key={r.id} style={[styles.tr, { opacity: 1 - i * 0.08 }]}>
              <Text style={[styles.tdType, { color: r.type === 'down' ? colors.cyan : colors.textMute }]}>
                {r.type === 'down' ? 'keydown' : 'keyup'}
                {r.repeat ? '*' : ''}
              </Text>
              <Text style={[styles.tdKey, { color: colors.gold }]} numberOfLines={1}>
                {JSON.stringify(pretty(r.key) === 'Space' ? ' ' : r.key)}
              </Text>
              <Text style={styles.tdCode} numberOfLines={1}>
                {r.code}
              </Text>
              {!compact ? (
                <Text style={styles.tdMods} numberOfLines={1}>
                  {r.mods.length ? r.mods.join(' + ') : '—'}
                </Text>
              ) : null}
            </View>
          ))
        )}
      </View>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  panel: {
    gap: 16,
    padding: 18,
    borderRadius: 16,
    backgroundColor: site.code,
    borderWidth: 1,
    borderColor: site.stroke,
    cursor: 'text',
  },
  ring: { position: 'absolute', left: -1, top: -1, right: -1, bottom: -1, borderRadius: 16, borderWidth: 2, borderColor: colors.cyan, boxShadow: '0 0 30px rgba(79,209,255,0.25)' },
  head: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  dot: { width: 8, height: 8, borderRadius: 4 },
  headText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.textDim },
  keys: { gap: 6, alignSelf: 'flex-start', width: '100%', maxWidth: 420 },
  keyRow: { flexDirection: 'row', gap: 6 },
  cap: {
    height: 32,
    borderRadius: 7,
    alignItems: 'center',
    justifyContent: 'center',
    overflow: 'hidden',
    backgroundColor: '#141c33',
    borderWidth: 1,
    borderColor: 'rgba(148,170,255,0.22)',
    borderBottomWidth: 3,
    borderBottomColor: 'rgba(0,0,0,0.6)',
  },
  capLit: { backgroundColor: colors.cyan, boxShadow: '0 0 18px rgba(79,209,255,0.8)' },
  capText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.textDim },
  table: { gap: 2, minHeight: 30 + 9 * 26 },
  tr: { flexDirection: 'row', alignItems: 'center', height: 24, gap: 10 },
  th: { borderBottomWidth: 1, borderBottomColor: site.stroke, marginBottom: 4 },
  thText: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, color: colors.textMute },
  tdType: { width: 74, fontFamily: mono, fontSize: 12 },
  tdKey: { width: 92, fontFamily: mono, fontSize: 12 },
  tdCode: { flex: 1, fontFamily: mono, fontSize: 12, color: colors.textDim },
  tdMods: { width: 120, fontFamily: mono, fontSize: 12, color: colors.textDim },
  empty: { fontFamily: font, fontSize: 13, color: colors.textMute, paddingTop: 6 },
});
