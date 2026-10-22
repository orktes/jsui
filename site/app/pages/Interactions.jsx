// /interactions: the AETHERFALL hub screen, live in a scaled <Stage>, plus a
// guide to everything in it worth poking at.
import React, { useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site, text } from '../theme.js';
import { Button, Card, Column, Key, Pill, Reveal, SectionHead, Stage, useLayout } from '../components/ui.jsx';
import { CodeBlock } from '../components/CodeBlock.jsx';
import Aetherfall from '../showcase/Aetherfall.jsx';

const TRY = [
  {
    title: 'Hover, press, focus',
    body: 'Menu buttons lean in on hover, squash on press and spring back. Three Animated values per button, blended with Animated.add, all on the native driver.',
    keys: ['Mouse', 'Tap'],
    api: 'Animated.spring · useNativeDriver',
    color: colors.gold,
  },
  {
    title: 'Gamepad spatial focus',
    body: 'Press Tab or a D-pad direction and focus walks the screen spatially: menu, carousel, recruit field, armory. A glowing ring springs in only for keyboard / gamepad focus.',
    keys: ['Tab', '↑', '↓', '←', '→', 'A'],
    api: 'focusable · onFocus focusVisible',
    color: colors.cyan,
  },
  {
    title: 'Snap carousel',
    body: 'Drag, swipe or wheel the featured cards. Scale, parallax, dimming and the page dots are interpolations of one scroll offset fed natively by Animated.event.',
    keys: ['Drag', 'Wheel', 'LB', 'RB'],
    api: 'snapToInterval · Animated.event',
    color: colors.orange,
  },
  {
    title: 'Momentum list',
    body: 'The armory is a virtualized FlatList with 48 rows. Fling it on touch, wheel it, or focus a row and hold a direction: it glides the focused row into view.',
    keys: ['Wheel', 'Fling', '↓'],
    api: 'FlatList · getItemLayout',
    color: colors.blue,
  },
  {
    title: 'Real text input',
    body: 'Click the recruit field and type a callsign. Caret, selection, word jumps, clipboard and undo are handled natively by the runtime. Enter adds a squad chip.',
    keys: ['Type', 'Ctrl+A', 'Ctrl+Z', 'Enter'],
    api: 'TextInput · onSubmitEditing',
    color: colors.green,
  },
  {
    title: 'LayoutAnimation chips',
    body: 'New chips grow in with a spring, removed ones fade out as ghosts while their siblings slide over. One configureNext call before setState.',
    keys: ['Enter', '×'],
    api: 'LayoutAnimation.configureNext',
    color: colors.purple,
  },
  {
    title: 'Spring modal',
    body: 'Continue or Deploy expedition springs a panel in over a fading backdrop. It traps focus like a native modal; Escape, B or a backdrop click closes it.',
    keys: ['Enter', 'Esc', 'B'],
    api: 'accessibilityViewIsModal · spring',
    color: colors.red,
  },
];

const SNIPPET = `
function MenuButton({ label, onPress }) {
  const hover = useRef(new Animated.Value(0)).current;
  const spring = (v) =>
    Animated.spring(hover, { toValue: v, stiffness: 340, damping: 20,
                             useNativeDriver: true }).start();
  return (
    <Pressable onPress={onPress}
               onHoverIn={() => spring(1)} onHoverOut={() => spring(0)}>
      <Animated.View style={[styles.button, {
        transform: [{ translateX: hover.interpolate({
          inputRange: [0, 1], outputRange: [0, 12] }) }],
      }]}>
        <Text style={styles.label}>{label}</Text>
      </Animated.View>
    </Pressable>
  );
}`;

function TryCard({ item, index, style }) {
  return (
    <Reveal delay={80 + index * 50} style={style}>
      <Card style={styles.tryCard}>
        <View style={styles.tryHead}>
          <View style={[styles.num, { borderColor: item.color + '88', backgroundColor: item.color + '1a' }]}>
            <Text style={[styles.numText, { color: item.color }]}>{index + 1}</Text>
          </View>
          <Text style={[text.h3, { flex: 1 }]}>{item.title}</Text>
        </View>
        <Text style={[text.body, { flex: 1 }]}>{item.body}</Text>
        <View style={styles.keys}>
          {item.keys.map((k) => (
            <Key key={k} color={item.color}>
              {k}
            </Key>
          ))}
        </View>
        <Text style={styles.api}>{item.api}</Text>
      </Card>
    </Reveal>
  );
}

export function InteractionsPage() {
  const { width, gutter, phone, tablet } = useLayout();
  const inner = Math.min(width - gutter * 2, 1240);
  const [run, setRun] = useState(0);
  const cols = phone ? 1 : tablet ? 2 : 3;
  const gap = phone ? 14 : 18;
  const cardW = (inner - gap * (cols - 1)) / cols;
  return (
    <Column style={{ paddingTop: phone ? 36 : 64, paddingBottom: 40 }}>
      <Reveal>
        <SectionHead
          eyebrow="Interactions"
          title="A whole game hub. Drive it with anything."
          lead="AETHERFALL is one 1280×720 React Native screen running live inside this page. Mouse, touch, keyboard and gamepad all work, and every animation in it is ordinary Animated or LayoutAnimation code."
        />
      </Reveal>

      <Reveal delay={120} style={{ marginTop: phone ? 24 : 36 }}>
        <Stage key={run} width={inner} radius={phone ? 12 : 18}>
          <Aetherfall active />
        </Stage>
        <View style={[styles.toolbar, phone && { flexDirection: 'column', alignItems: 'flex-start' }]}>
          <View style={styles.liveRow}>
            <View style={styles.liveDot} />
            <Text style={styles.liveText}>Live · scaled to {Math.round((inner / 1280) * 100)}% · hit testing follows the transform</Text>
          </View>
          <View style={{ flex: phone ? 0 : 1 }} />
          <Button small kind="ghost" label="Restart screen" icon="↻" onPress={() => setRun((r) => r + 1)} />
        </View>
        {phone ? (
          <Text style={[text.small, { marginTop: 10 }]}>Tip: the screen is laid out for 1280×720, so it is easiest to explore on a larger display or in landscape.</Text>
        ) : null}
      </Reveal>

      <View style={{ marginTop: phone ? 48 : 80 }}>
        <SectionHead eyebrow="What to try" title="Seven things hiding in one screen" lead="Each of these is a few lines of React Native. Nothing is special-cased for the web: the same bundle runs in Godot or your own engine." />
      </View>
      <View style={[styles.grid, { gap, marginTop: phone ? 24 : 36 }]}>
        {TRY.map((item, i) => (
          <TryCard key={item.title} item={item} index={i} style={{ width: cardW }} />
        ))}
        <Reveal delay={80 + TRY.length * 50} style={{ width: cols === 3 ? cardW * 2 + gap : cardW }}>
          <Card style={[styles.tryCard, { gap: 14 }]}>
            <Text style={text.h3}>Input map</Text>
            <View style={styles.mapRow}>
              <MapCol title="Keyboard" color={colors.cyan} rows={[['Tab', 'Next focus'], ['↑↓←→', 'Spatial focus'], ['Enter', 'Activate'], ['Esc', 'Close panel']]} />
              <MapCol title="Gamepad" color={colors.gold} rows={[['D-pad', 'Spatial focus'], ['A', 'Activate'], ['B', 'Close panel'], ['LB / RB', 'Page carousel']]} />
              {phone ? null : <MapCol title="Pointer" color={colors.green} rows={[['Hover', 'Glow + lean'], ['Press', 'Squash'], ['Drag', 'Scroll / snap'], ['Wheel', 'Scroll']]} />}
            </View>
          </Card>
        </Reveal>
      </View>

      <View style={[styles.split, tablet && { flexDirection: 'column' }, { marginTop: phone ? 48 : 80 }]}>
        <View style={{ flex: tablet ? 0 : 1, gap: 16 }}>
          <SectionHead eyebrow="Under the hood" title="Springs that never touch JS per frame" />
          <Text style={text.body}>
            Starting an animation with useNativeDriver: true serializes the value graph (springs, interpolations, additions) into the C++ runtime once. From then on the runtime steps it every tick and writes the view styles itself, repainting only the rectangles that changed. JS hears back only when the animation ends.
          </Text>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
            <Pill color={colors.gold}>Animated.spring</Pill>
            <Pill color={colors.cyan}>Animated.event</Pill>
            <Pill color={colors.purple}>LayoutAnimation</Pill>
            <Pill color={colors.green}>FlatList</Pill>
          </View>
          <Button label="See the motion lab" kind="ghost" to="/motion" style={{ marginTop: 6 }} />
        </View>
        <View style={{ flex: tablet ? 0 : 1.2 }}>
          <CodeBlock title="MenuButton.jsx" code={SNIPPET} />
        </View>
      </View>
    </Column>
  );
}

function MapCol({ title, color, rows }) {
  return (
    <View style={{ flex: 1, gap: 8, minWidth: 140 }}>
      <Text style={[styles.mapTitle, { color }]}>{title}</Text>
      {rows.map(([k, label]) => (
        <View key={k} style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <Key color={color}>{k}</Key>
          <Text style={text.small}>{label}</Text>
        </View>
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  toolbar: { flexDirection: 'row', alignItems: 'center', gap: 12, marginTop: 14 },
  liveRow: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  liveDot: { width: 8, height: 8, borderRadius: 4, backgroundColor: colors.green, boxShadow: '0 0 10px rgba(74,222,128,0.9)' },
  liveText: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textDim },
  grid: { flexDirection: 'row', flexWrap: 'wrap' },
  tryCard: { flex: 1, gap: 12, padding: 22 },
  tryHead: { flexDirection: 'row', alignItems: 'center', gap: 12 },
  num: { width: 30, height: 30, borderRadius: 9, borderWidth: 1, alignItems: 'center', justifyContent: 'center' },
  numText: { fontFamily: font, fontWeight: '700', fontSize: 14 },
  keys: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  api: { fontFamily: mono, fontSize: 12, color: colors.textMute, borderTopWidth: 1, borderTopColor: site.stroke, paddingTop: 10 },
  mapRow: { flexDirection: 'row', gap: 18, flexWrap: 'wrap' },
  mapTitle: { fontFamily: font, fontWeight: '700', fontSize: 12, letterSpacing: 1.6, textTransform: 'uppercase' },
  split: { flexDirection: 'row', gap: 40, alignItems: 'flex-start' },
});
