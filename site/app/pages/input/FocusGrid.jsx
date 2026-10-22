// Spatial focus demo: a level-select grid. Arrows / D-pad / stick move focus
// spatially (native InputManager), Tab walks in order, Enter / A selects.
// Rows wrap around through nextFocusLeft / nextFocusRight overrides (node
// ids from findNodeHandle), and the locked level is skipped (disabled).
import React, { memo, useEffect, useRef, useState } from 'react';
import { Animated, Pressable, StyleSheet, Text, View, findNodeHandle } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';

const LEVELS = [
  ['Ember Gate', 3, colors.orange],
  ['Saltmarsh', 3, colors.cyan],
  ['Gloam Wood', 2, colors.green],
  ['Iron Steppe', 3, colors.gold],
  ['Frost Hollow', 1, colors.blue],
  ['Sunken Bell', 2, colors.purple],
  ['Ash Choir', 0, colors.red],
  ['Locked', 0, colors.textMute, true],
  ['Mirror Sea', 2, colors.cyan],
  ['Thorn Keep', 1, colors.green],
  ['Starfall', 3, colors.gold],
  ['The Rift', 0, colors.purple],
];

const spring = (v, to, cfg) => Animated.spring(v, { toValue: to, stiffness: 300, damping: 18, mass: 0.8, useNativeDriver: true, ...cfg });

const Tile = memo(function Tile({ index, name, stars, color, locked, selected, onFocusTile, onSelect, setRef, nextLeft, nextRight, wrapL, wrapR, h }) {
  const f = useRef(new Animated.Value(0)).current;
  const p = useRef(new Animated.Value(0)).current;
  const [hot, setHot] = useState(false);
  const lift = (on) => spring(f, on ? 1 : 0).start();
  const scale = Animated.add(
    f.interpolate({ inputRange: [0, 1], outputRange: [1, 1.07] }),
    p.interpolate({ inputRange: [0, 1], outputRange: [0, -0.08] }),
  );
  return (
    <Pressable
      ref={setRef}
      disabled={locked}
      nextFocusLeft={nextLeft}
      nextFocusRight={nextRight}
      onFocus={(e) => {
        setHot(true);
        lift(true);
        onFocusTile(index, !!(e.nativeEvent && e.nativeEvent.focusVisible));
      }}
      onBlur={() => {
        setHot(false);
        lift(false);
      }}
      onHoverIn={() => lift(true)}
      onHoverOut={() => !hot && lift(false)}
      onPressIn={() => spring(p, 1, { stiffness: 700, damping: 24 }).start()}
      onPressOut={() => spring(p, 0, { stiffness: 300, damping: 12 }).start()}
      onPress={() => onSelect(index)}
      style={{ flex: 1 }}
    >
      <Animated.View style={[styles.tile, { height: h, zIndex: hot ? 2 : 0, transform: [{ scale }, { translateY: f.interpolate({ inputRange: [0, 1], outputRange: [0, -3] }) }] }]}>
        <View style={[StyleSheet.absoluteFill, styles.tileBg, { experimental_backgroundImage: `radial-gradient(circle at 30% 0%, ${color}38 0%, rgba(15,22,40,0) 70%)` }]} />
        <Animated.View pointerEvents="none" style={[styles.ring, { borderColor: color, boxShadow: `0 0 26px ${color}88`, opacity: f }]} />
        <View style={styles.tileTop}>
          <Text style={[styles.num, { color: locked ? colors.textMute : color }]}>{String(index + 1).padStart(2, '0')}</Text>
          {selected ? (
            <View style={[styles.check, { backgroundColor: color }]}>
              <Text style={styles.checkText}>✓</Text>
            </View>
          ) : null}
        </View>
        <Text style={[styles.name, locked && { color: colors.textMute }]} numberOfLines={1}>
          {name}
        </Text>
        <View style={styles.stars}>
          {[0, 1, 2].map((i) => (
            <Text key={i} style={[styles.star, { color: i < stars ? colors.gold : 'rgba(148,170,255,0.2)' }]}>
              ★
            </Text>
          ))}
        </View>
        {wrapL ? <Text style={[styles.wrap, { right: 10 }]}>↺</Text> : null}
        {wrapR ? <Text style={[styles.wrap, { right: 10 }]}>↻</Text> : null}
      </Animated.View>
    </Pressable>
  );
});

export function FocusGrid({ cols = 4 }) {
  const refs = useRef([]);
  const setters = useRef(LEVELS.map((_, i) => (r) => (refs.current[i] = r))).current;
  const [ids, setIds] = useState([]);
  const [focus, setFocus] = useState(null);
  const [selected, setSelected] = useState(0);

  useEffect(() => {
    setIds(refs.current.map((r) => findNodeHandle(r)));
  }, [cols]);

  const rows = [];
  for (let i = 0; i < LEVELS.length; i += cols) rows.push(LEVELS.slice(i, i + cols).map((l, j) => [i + j, l]));

  // Row wrap-around: the last tile's "right" goes to the row's first tile and back.
  const target = (i) => (LEVELS[i] && !LEVELS[i][3] ? ids[i] : undefined);
  const onFocusTile = useRef((index, visible) => setFocus({ index, visible })).current;
  const onSelect = useRef((i) => setSelected(i)).current;

  const f = focus && LEVELS[focus.index];
  return (
    <View style={styles.wrapBox}>
      <View style={{ gap: 10 }}>
        {rows.map((row, r) => (
          <View key={r} style={styles.row}>
            {row.map(([i, [name, stars, color, locked]], c) => {
              const first = row[0][0];
              const last = row[row.length - 1][0];
              const isLast = c === row.length - 1;
              return (
                <Tile
                  key={i}
                  index={i}
                  name={name}
                  stars={stars}
                  color={color}
                  locked={locked}
                  selected={selected === i}
                  h={cols > 3 ? 96 : 88}
                  setRef={setters[i]}
                  nextRight={isLast ? target(first) : undefined}
                  nextLeft={c === 0 ? target(last) : undefined}
                  wrapR={isLast && !locked}
                  wrapL={c === 0 && !locked}
                  onFocusTile={onFocusTile}
                  onSelect={onSelect}
                />
              );
            })}
          </View>
        ))}
      </View>
      <View style={styles.readout}>
        <Text style={styles.readLabel}>FOCUS</Text>
        <Text style={styles.readValue} numberOfLines={1}>
          {f ? `${String(focus.index + 1).padStart(2, '0')} · ${f[0]}` : 'nothing yet'}
        </Text>
        {f ? (
          <View style={[styles.via, { borderColor: (focus.visible ? colors.cyan : colors.gold) + '66' }]}>
            <Text style={[styles.viaText, { color: focus.visible ? colors.cyan : colors.gold }]}>{focus.visible ? 'keys / gamepad' : 'pointer'}</Text>
          </View>
        ) : null}
        <View style={{ flex: 1 }} />
        <Text style={styles.readLabel}>SELECTED</Text>
        <Text style={styles.readValue}>{LEVELS[selected][0]}</Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  wrapBox: { gap: 16 },
  row: { flexDirection: 'row', gap: 10 },
  tile: { borderRadius: 14, padding: 12, justifyContent: 'space-between' },
  tileBg: { borderRadius: 14, backgroundColor: '#0f1628', borderWidth: 1, borderColor: site.stroke },
  ring: { position: 'absolute', left: -1, top: -1, right: -1, bottom: -1, borderRadius: 15, borderWidth: 2 },
  tileTop: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  num: { fontFamily: mono, fontWeight: '700', fontSize: 12 },
  check: { width: 18, height: 18, borderRadius: 9, alignItems: 'center', justifyContent: 'center' },
  checkText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: '#0b1020' },
  name: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
  stars: { flexDirection: 'row', gap: 2 },
  star: { fontFamily: font, fontSize: 12 },
  wrap: { position: 'absolute', bottom: 10, fontFamily: font, fontSize: 13, color: 'rgba(148,170,255,0.45)' },
  readout: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    height: 40,
    paddingHorizontal: 14,
    borderRadius: 10,
    backgroundColor: 'rgba(10,15,29,0.8)',
    borderWidth: 1,
    borderColor: site.stroke,
  },
  readLabel: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, color: colors.textMute },
  readValue: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text, flexShrink: 1 },
  via: { paddingHorizontal: 8, height: 20, borderRadius: 10, borderWidth: 1, justifyContent: 'center' },
  viaText: { fontFamily: font, fontWeight: '600', fontSize: 11 },
});
