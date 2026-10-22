// LayoutAnimation: insert, delete, shuffle, sort and switch list <-> grid.
// Every change is a plain setState preceded by LayoutAnimation.configureNext;
// the runtime animates the layout diff (and fades removed rows as ghosts).
import React, { useState } from 'react';
import { LayoutAnimation, Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';
import { DemoButton, DemoCard, Segmented } from './controls.jsx';

const POOL = [
  ['Slay the Ember Wyrm', colors.orange, 'Raid'],
  ['Chart the Verdant Reach', colors.green, 'Explore'],
  ['Win 3 ranked matches', colors.purple, 'PvP'],
  ['Collect 12 Tide Pearls', colors.cyan, 'Gather'],
  ['Forge a legendary blade', colors.gold, 'Craft'],
  ['Rescue the lost caravan', colors.red, 'Story'],
  ['Tame a frost drake', colors.blue, 'Hunt'],
  ['Decode the Void Codex', colors.purple, 'Lore'],
  ['Defend the north gate', colors.orange, 'Siege'],
  ['Brew 5 phoenix draughts', colors.red, 'Alchemy'],
];

const PRESETS = {
  spring: { duration: 520, create: { type: 'spring', property: 'scaleXY', springDamping: 0.6 }, update: { type: 'spring', springDamping: 0.72 }, delete: { type: 'easeOut', property: 'opacity', duration: 220 } },
  easeInEaseOut: { duration: 360, create: { type: 'easeInEaseOut', property: 'opacity' }, update: { type: 'easeInEaseOut' }, delete: { type: 'easeInEaseOut', property: 'opacity' } },
  linear: { duration: 600, create: { type: 'linear', property: 'scaleXY' }, update: { type: 'linear' }, delete: { type: 'linear', property: 'scaleXY' } },
};

let nextId = 100;
const make = (i) => ({ id: nextId++, title: POOL[i % POOL.length][0], color: POOL[i % POOL.length][1], tag: POOL[i % POOL.length][2], xp: 150 + ((i * 137) % 9) * 50 });

export function LayoutLab() {
  const [items, setItems] = useState(() => [0, 1, 2, 3, 4].map(make));
  const [grid, setGrid] = useState(false);
  const [preset, setPreset] = useState('spring');
  const [cursor, setCursor] = useState(5);
  const animate = (fn) => {
    LayoutAnimation.configureNext(PRESETS[preset]);
    fn();
  };
  const add = () =>
    animate(() => {
      const it = make(cursor);
      setCursor((c) => c + 1);
      setItems((xs) => {
        const at = Math.min(xs.length, 1 + ((cursor * 3) % (xs.length + 1)));
        const out = xs.slice();
        out.splice(at, 0, it);
        return out.slice(0, 9);
      });
    });
  const remove = (id) => animate(() => setItems((xs) => xs.filter((x) => x.id !== id)));
  const shuffle = () =>
    animate(() =>
      setItems((xs) => {
        const out = xs.slice();
        for (let i = out.length - 1; i > 0; i--) {
          const j = Math.floor(Math.random() * (i + 1));
          [out[i], out[j]] = [out[j], out[i]];
        }
        return out;
      }),
    );
  const sort = () => animate(() => setItems((xs) => xs.slice().sort((a, b) => b.xp - a.xp)));
  const toggle = () => animate(() => setGrid((g) => !g));

  return (
    <DemoCard
      index={5}
      title="LayoutAnimation"
      blurb="No animated values at all: call LayoutAnimation.configureNext, then change state. The runtime diffs the next layout, springs every moved or resized node to its new frame, grows new ones in and keeps removed ones as fading ghosts while their siblings reflow."
      tags={[['LayoutAnimation', colors.purple], ['create / update / delete', colors.gold]]}
      codeTitle="quest-log.jsx"
      code={`
const remove = (id) => {
  LayoutAnimation.configureNext({
    duration: 520,
    create: { type: 'spring', property: 'scaleXY',
              springDamping: 0.6 },
    update: { type: 'spring', springDamping: 0.72 },
    delete: { type: 'easeOut', property: 'opacity' },
  });
  setQuests((qs) => qs.filter((q) => q.id !== id));
};

// List <-> grid is just a style change, animated too.
<View style={grid ? styles.grid : styles.list}>
  {quests.map((q) => <Quest key={q.id} {...q} />)}
</View>`}
    >
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8, alignItems: 'center' }}>
        <DemoButton primary label="Add" icon="+" onPress={add} />
        <DemoButton label="Shuffle" icon="↻" color={colors.cyan} onPress={shuffle} />
        <DemoButton label="Sort by XP" icon="↓" color={colors.cyan} onPress={sort} />
        <DemoButton label={grid ? 'List' : 'Grid'} color={colors.purple} onPress={toggle} />
        <View style={{ flex: 1 }} />
        <Segmented options={Object.keys(PRESETS)} value={preset} onChange={setPreset} color={colors.gold} small />
      </View>
      <View style={[grid ? styles.grid : styles.list, { minHeight: 300 }]}>
        {items.map((q) => (
          <View key={q.id} style={grid ? styles.cellGrid : styles.cellList}>
            <View style={[styles.quest, grid && styles.questGrid, { borderColor: q.color + '55' }]}>
              <View style={[styles.stripe, { backgroundColor: q.color }, grid && styles.stripeGrid]} />
              <View style={{ flex: 1, gap: 3, minWidth: 0 }}>
                <Text style={[styles.tag, { color: q.color }]}>{q.tag}</Text>
                <Text style={styles.title} numberOfLines={grid ? 2 : 1}>
                  {q.title}
                </Text>
              </View>
              <Text style={[styles.xp, grid && styles.xpGrid]}>{q.xp} XP</Text>
              <Pressable onPress={() => remove(q.id)} hitSlop={6} style={({ hovered, focused, pressed }) => [styles.x, (hovered || focused) && styles.xHot, pressed && { transform: [{ scale: 0.9 }] }, { cursor: 'pointer' }]}>
                <Text style={styles.xText}>×</Text>
              </Pressable>
            </View>
          </View>
        ))}
        {items.length === 0 ? <Text style={styles.empty}>Quest log empty. Press Add.</Text> : null}
      </View>
    </DemoCard>
  );
}

const styles = StyleSheet.create({
  list: { gap: 8 },
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  cellList: {},
  cellGrid: { width: '31.5%', flexGrow: 1, minWidth: 130 },
  quest: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    height: 52,
    paddingRight: 8,
    borderRadius: 10,
    borderWidth: 1,
    backgroundColor: 'rgba(16,23,42,0.9)',
    overflow: 'hidden',
  },
  questGrid: { height: 108, alignItems: 'flex-start', paddingTop: 12, paddingLeft: 14, paddingRight: 10 },
  stripe: { width: 4, alignSelf: 'stretch' },
  stripeGrid: { position: 'absolute', left: 0, top: 0, bottom: 0 },
  tag: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.4, textTransform: 'uppercase' },
  title: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
  xpGrid: { position: 'absolute', left: 14, bottom: 12 },
  xp: { fontFamily: mono, fontWeight: '700', fontSize: 12, color: colors.goldLight },
  x: { width: 26, height: 26, borderRadius: 13, alignItems: 'center', justifyContent: 'center' },
  xHot: { backgroundColor: 'rgba(255,77,106,0.3)' },
  xText: { fontFamily: font, fontWeight: '600', fontSize: 17, color: colors.textDim, marginTop: -2 },
  empty: { fontFamily: font, fontSize: 14, color: colors.textMute, padding: 20 },
});
