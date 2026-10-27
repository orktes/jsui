// Shared HUD of the engine examples (integrations/raylib, sdl3, sokol). Each
// host runs a small game scene and this UI on top of it, through the same C
// API:
//   * Game -> UI shared state `game` (jsui_set_state): engine, renderer, fps,
//     entity count, paused, the selected unit (`selected`: its index in
//     `entities`, -1 for none) and what the game received because the UI did
//     not consume it (`passed`: clicks, wheel, keys, pad).
//   * Game -> UI frame data `entities` (jsui_set_buffer, Float32Array of
//     x, y, visible, hue per entity in logical px, every frame): one nameplate
//     per entity, moved by the runtime with no JS per frame
//     (useHostBufferValue).
//   * UI -> game messages (Host.postMessage): game:spawn {count}, game:clear,
//     game:pause {paused}, game:quit, ui:anchors (where the Spawn button is,
//     for the hosts' --selftest) and ui:selected {index} (the selection the
//     UI shows, sent when it changes, so a --selftest can check it).
//   * A native component: the radar (requireNativeComponent('Radar')) is
//     drawn by the game's own code (integrations/shared/radar.h) from its
//     units every frame. The HUD lays it out, pulses its `flash` prop with
//     the native driver, and sends taps to it as a `select` command; the
//     radar selects the unit in the game and answers with onSelect. Hosts
//     without the class (Unity, Unreal) leave it out (hasNativeComponent).
// Small screens (the Three.js host on a phone, site/demo): under 640 px
// wide or 500 px tall the HUD packs tighter (useWindowDimensions) and keeps
// out of the safe-area insets the host publishes; the hosts' 1280x720
// windows look as before. `game.hints` replaces the hint line (touch hosts).
// Input pass-through: the root and the nameplates take no input, so clicks,
// drags and wheel on empty parts of the screen are not consumed and the host
// hands them to the game (select a unit, camera orbit, zoom, spawn). The
// game picks the unit under a click with its own raycast. The pause menu's
// backdrop claims presses (onStartShouldSetResponder), so nothing reaches
// the game behind it.
import React, { memo, useEffect, useMemo, useRef } from 'react';
import {
  Animated,
  AppRegistry,
  Host,
  Pressable,
  StyleSheet,
  Text,
  View,
  codegenNativeCommands,
  hasNativeComponent,
  requireNativeComponent,
  useHostBufferSelector,
  useHostBufferValue,
  useHostState,
  useWindowDimensions,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { colors, font, panel } from '../../../examples/theme.js';

const MAX_PLATES = 48;
const STRIDE = 4; // x, y, visible, hue
const countOf = (b) => Math.min(MAX_PLATES, Math.floor(b.length / STRIDE));

function hueColor(i) {
  const palette = [colors.gold, colors.cyan, colors.purple, colors.green, colors.orange, colors.blue, colors.red];
  return palette[i % palette.length];
}

const unitName = (index) => `UNIT ${String(index + 1).padStart(2, '0')}`;

function Nameplate({ index, selected }) {
  const x = useHostBufferValue('entities', index * STRIDE);
  const y = useHostBufferValue('entities', index * STRIDE + 1);
  const visible = useHostBufferValue('entities', index * STRIDE + 2);
  const c = hueColor(index);
  return (
    <Animated.View
      pointerEvents="none"
      style={[
        styles.plate,
        selected && styles.plateSelected,
        { opacity: visible, transform: [{ translateX: x }, { translateY: y }] },
      ]}
    >
      {selected ? <Text style={[styles.plateTag, { color: c }]}>SELECTED</Text> : null}
      <View
        style={[
          styles.plateInner,
          selected && { borderColor: c, backgroundColor: 'rgba(16, 22, 40, 0.94)', boxShadow: `0 0 14px ${c}` },
        ]}
      >
        <View style={[styles.plateDot, { backgroundColor: c, boxShadow: `0 0 8px ${c}` }]} />
        <Text style={[styles.plateText, selected && styles.plateTextSelected]}>{unitName(index)}</Text>
      </View>
      <View style={styles.plateBar}>
        <View style={[styles.plateFill, { width: `${40 + ((index * 37) % 60)}%`, backgroundColor: c }]} />
      </View>
    </Animated.View>
  );
}

const Nameplates = memo(function Nameplates({ selected }) {
  const count = useHostBufferSelector('entities', countOf, 0);
  const plates = [];
  // The selected plate last: drawn over the others.
  for (let i = 0; i < count; i++) if (i !== selected) plates.push(<Nameplate key={i} index={i} />);
  if (selected >= 0 && selected < count) plates.push(<Nameplate key={selected} index={selected} selected />);
  return (
    <View pointerEvents="none" style={StyleSheet.absoluteFill}>
      {plates}
    </View>
  );
});

const Stat = memo(function Stat({ label, value, color, dense }) {
  return (
    <View style={styles.stat}>
      <Text style={styles.statLabel}>{label}</Text>
      <Text style={[styles.statValue, dense && styles.statValueDense, color && { color }]}>{value}</Text>
    </View>
  );
});

function EnginePanel({ game, layout }) {
  const passed = game.passed || {};
  const { narrow, dense, insets } = layout;
  return (
    <View
      pointerEvents="none"
      style={[
        styles.enginePanel,
        dense && styles.enginePanelDense,
        dense && { left: 12 + insets.left, top: 12 + insets.top },
        narrow && { width: undefined, right: 12 + insets.right },
      ]}
    >
      <View style={styles.engineHead}>
        <View style={styles.liveDot} />
        <Text style={styles.engineName}>{game.engine || 'host'}</Text>
      </View>
      <Text style={styles.renderer}>{game.renderer || ''}</Text>
      <View style={[styles.statRow, dense && styles.statRowDense]}>
        <Stat label="FPS" value={game.fps != null ? String(game.fps) : '–'} color={colors.green} dense={dense} />
        <Stat label="Units" value={String(game.entities ?? 0)} color={colors.gold} dense={dense} />
        <Stat label="State" value={game.paused ? 'Paused' : 'Live'} color={game.paused ? colors.orange : colors.cyan} dense={dense} />
      </View>
      <View style={[styles.divider, dense && styles.dividerDense]} />
      <Text style={[styles.sectionLabel, dense && styles.sectionLabelDense]}>Passed through to the game</Text>
      <View style={[styles.statRow, dense && styles.statRowDense]}>
        <Stat label="Clicks" value={String(passed.clicks ?? 0)} dense={dense} />
        <Stat label="Wheel" value={String(passed.wheel ?? 0)} dense={dense} />
        <Stat label="Keys" value={String(passed.keys ?? 0)} dense={dense} />
        <Stat label="Pad" value={String(passed.pad ?? 0)} dense={dense} />
      </View>
      <View style={[styles.divider, dense && styles.dividerDense]} />
      <Text style={[styles.sectionLabel, dense && styles.sectionLabelDense]}>Selected</Text>
      <Selection index={game.selected ?? -1} />
    </View>
  );
}

const Selection = memo(function Selection({ index }) {
  if (index < 0) return <Text style={styles.selectionNone}>None · click a unit</Text>;
  const c = hueColor(index);
  return (
    <View style={styles.selection}>
      <View style={[styles.selectionDot, { backgroundColor: c, boxShadow: `0 0 10px ${c}` }]} />
      <Text style={[styles.selectionName, { color: c }]}>{unitName(index)}</Text>
    </View>
  );
});

const Radar = requireNativeComponent('Radar');
const RadarCommands = codegenNativeCommands({ supportedCommands: ['select'] });
const RADAR_SIZE = 184;

// The game draws it (shared/radar.h); the HUD only places it and talks to it.
const RadarPanel = memo(function RadarPanel({ selected }) {
  const ref = useRef(null);
  const flash = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    if (selected < 0) return;
    flash.setValue(1);
    Animated.timing(flash, { toValue: 0, duration: 700, useNativeDriver: true }).start();
  }, [selected]);
  if (!hasNativeComponent('Radar')) return null;
  return (
    <View style={styles.radarPanel}>
      <Text style={styles.sectionLabel}>Radar · native component</Text>
      <Pressable
        style={styles.radarPress}
        onPress={(e) => RadarCommands.select(ref.current, e.nativeEvent.locationX, e.nativeEvent.locationY, RADAR_SIZE)}
      >
        <Radar ref={ref} flash={flash} accent={colors.cyan} style={styles.radar} />
      </Pressable>
    </View>
  );
});

function ActionButton({ label, hint, onPress, accent, innerRef, onLayout, autoFocus, style }) {
  return (
    <Pressable
      ref={innerRef}
      onLayout={onLayout}
      onPress={onPress}
      autoFocus={autoFocus}
      style={({ pressed, focused, hovered }) => [
        styles.action,
        style,
        (focused || hovered) && { borderColor: accent, backgroundColor: 'rgba(255, 255, 255, 0.1)' },
        pressed && styles.actionPressed,
      ]}
    >
      <Text style={styles.actionLabel}>{label}</Text>
      {hint ? <Text style={styles.actionHint}>{hint}</Text> : null}
    </Pressable>
  );
}

const ActionBar = memo(function ActionBar({ paused, layout }) {
  const spawnRef = useRef(null);
  const reportAnchors = () =>
    spawnRef.current?.measureInWindow((x, y, width, height) =>
      Host.postMessage('ui:anchors', { spawn: { x, y, width, height } }),
    );
  const { narrow, dense, insets } = layout;
  // Narrow: a 2 x 2 grid across the bottom.
  const button = narrow ? styles.actionNarrow : dense ? styles.actionDense : null;
  return (
    <View
      style={[
        styles.actionBar,
        dense && { bottom: 12 + insets.bottom, padding: 8, gap: 8 },
        narrow && { ...styles.actionBarNarrow, left: 12 + insets.left, right: 12 + insets.right },
      ]}
    >
      <ActionButton
        style={button}
        innerRef={spawnRef}
        onLayout={reportAnchors}
        label="Spawn"
        hint="+1 unit"
        accent={colors.gold}
        onPress={() => Host.postMessage('game:spawn', { count: 1 })}
      />
      <ActionButton
        style={button}
        label="Spawn ×10"
        accent={colors.gold}
        onPress={() => Host.postMessage('game:spawn', { count: 10 })}
      />
      <ActionButton style={button} label="Clear" accent={colors.red} onPress={() => Host.postMessage('game:clear', {})} />
      <ActionButton
        style={button}
        label={paused ? 'Resume' : 'Pause'}
        hint="Esc / Start"
        accent={colors.cyan}
        onPress={() => Host.postMessage('game:pause', { paused: !paused })}
      />
    </View>
  );
});

const Hints = memo(function Hints({ text, layout }) {
  const { narrow, dense, insets } = layout;
  // Short screens: no room between the panel and the action bar.
  if (dense && !narrow) return null;
  return (
    <View
      pointerEvents="none"
      style={[styles.hints, narrow && { bottom: 136 + insets.bottom, left: 16 + insets.left, right: 16 + insets.right }]}
    >
      <Text style={[styles.hintText, narrow && styles.hintTextNarrow]}>
        {text ||
          'Click a unit to select it · drag empty space to move the camera · wheel to zoom · Space spawns · Tab / D-pad moves focus · Enter / A presses'}
      </Text>
    </View>
  );
});

const PauseMenu = memo(function PauseMenu({ paused, engine, width }) {
  const t = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    Animated.spring(t, { toValue: paused ? 1 : 0, stiffness: 240, damping: 22, useNativeDriver: true }).start();
  }, [paused]);
  if (!paused) return null;
  return (
    // Claims presses: nothing behind the menu reaches the game while paused.
    <View style={styles.backdrop} onStartShouldSetResponder={() => true}>
      <Animated.View
        style={[
          styles.menu,
          width < 452 && { width: width - 32, padding: 22 },
          { opacity: t, transform: [{ scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.92, 1] }) }] },
        ]}
      >
        <Text style={styles.menuEyebrow}>{(engine || 'host').toUpperCase()}</Text>
        <Text style={styles.menuTitle}>Paused</Text>
        <Text style={styles.menuBody}>
          The game loop keeps rendering; the simulation is stopped. Input over the menu stays in the UI.
        </Text>
        <View style={styles.menuButtons}>
          <ActionButton
            label="Resume"
            accent={colors.cyan}
            autoFocus
            onPress={() => Host.postMessage('game:pause', { paused: false })}
          />
          <ActionButton label="Quit" accent={colors.red} onPress={() => Host.postMessage('game:quit', {})} />
        </View>
      </Animated.View>
    </View>
  );
});

function App() {
  const game = useHostState('game', {});
  const selected = game.selected ?? -1;
  const { width, height } = useWindowDimensions();
  const insets = useSafeAreaInsets();
  const narrow = width < 640;
  const dense = narrow || height < 500;
  // One object per change, so the memo'd parts re-render only then.
  const layout = useMemo(() => ({ narrow, dense, insets }), [narrow, dense, insets]);
  useEffect(() => Host.postMessage('ui:ready', {}), []);
  // Tell the game what the UI now shows as selected (its --selftest checks it).
  useEffect(() => Host.postMessage('ui:selected', { index: selected }), [selected]);
  return (
    // No background and no handlers on the root: the game shows through and
    // input on empty areas is not consumed.
    <View style={styles.root}>
      <Nameplates selected={selected} />
      <EnginePanel game={game} layout={layout} />
      <RadarPanel selected={selected} />
      <Hints text={game.hints} layout={layout} />
      <ActionBar paused={!!game.paused} layout={layout} />
      <PauseMenu paused={!!game.paused} engine={game.engine} width={width} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1 },
  plate: { position: 'absolute', left: -48, top: -44, width: 96, alignItems: 'center' },
  plateInner: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
    paddingHorizontal: 8,
    height: 20,
    borderRadius: 10,
    backgroundColor: 'rgba(7, 10, 20, 0.78)',
    borderWidth: 1,
    borderColor: colors.line,
  },
  plateDot: { width: 6, height: 6, borderRadius: 3 },
  plateText: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.2, color: colors.text },
  plateBar: { marginTop: 3, width: 56, height: 3, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.12)', overflow: 'hidden' },
  plateFill: { height: 3, borderRadius: 2 },
  plateSelected: { top: -60 },
  plateTag: { fontFamily: font, fontWeight: '800', fontSize: 9, letterSpacing: 1.8, marginBottom: 3, textShadow: '0 1px 4px rgba(0,0,0,0.9)' },
  plateTextSelected: { color: '#ffffff' },
  selection: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 6 },
  selectionDot: { width: 8, height: 8, borderRadius: 4 },
  selectionName: { fontFamily: font, fontWeight: '700', fontSize: 16, letterSpacing: 1.2 },
  selectionNone: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textMute, marginTop: 6 },
  enginePanel: { ...panel, position: 'absolute', left: 20, top: 20, width: 300, padding: 16 },
  enginePanelDense: { padding: 12, paddingTop: 10 },
  engineHead: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  liveDot: { width: 8, height: 8, borderRadius: 4, backgroundColor: colors.green, boxShadow: '0 0 8px rgba(74,222,128,0.9)' },
  engineName: { fontFamily: font, fontWeight: '700', fontSize: 16, letterSpacing: 0.5, color: colors.text },
  renderer: { fontFamily: font, fontWeight: '500', fontSize: 11, color: colors.textMute, marginTop: 4 },
  statRow: { flexDirection: 'row', gap: 16, marginTop: 12 },
  stat: { minWidth: 44 },
  statLabel: { fontFamily: font, fontWeight: '600', fontSize: 10, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textMute },
  statValue: { fontFamily: font, fontWeight: '700', fontSize: 18, color: colors.text, marginTop: 2 },
  statValueDense: { fontSize: 15, marginTop: 1 },
  statRowDense: { marginTop: 7 },
  dividerDense: { marginTop: 10 },
  sectionLabelDense: { marginTop: 9 },
  divider: { height: 1, backgroundColor: colors.line, marginTop: 14 },
  sectionLabel: { fontFamily: font, fontWeight: '600', fontSize: 10, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textDim, marginTop: 12 },
  radarPanel: { ...panel, position: 'absolute', right: 20, top: 20, padding: 14, paddingTop: 0, alignItems: 'center' },
  radarPress: { marginTop: 10, width: RADAR_SIZE, height: RADAR_SIZE, borderRadius: RADAR_SIZE / 2 },
  radar: { width: RADAR_SIZE, height: RADAR_SIZE, borderRadius: RADAR_SIZE / 2 },
  hints: { position: 'absolute', left: 0, right: 0, bottom: 92, alignItems: 'center' },
  hintText: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textDim, textShadow: '0 1px 4px rgba(0,0,0,0.9)' },
  hintTextNarrow: { textAlign: 'center', lineHeight: 17 },
  actionBar: {
    ...panel,
    position: 'absolute',
    bottom: 20,
    alignSelf: 'center',
    flexDirection: 'row',
    gap: 10,
    padding: 10,
    borderRadius: 18,
  },
  action: {
    minWidth: 112,
    height: 46,
    paddingHorizontal: 16,
    borderRadius: 12,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 'rgba(148, 170, 255, 0.08)',
    borderWidth: 1,
    borderColor: colors.line,
  },
  actionBarNarrow: { alignSelf: undefined, flexWrap: 'wrap' },
  actionNarrow: { flexGrow: 1, flexBasis: '40%', minWidth: 0, height: 44, paddingHorizontal: 8 },
  actionDense: { minWidth: 96, height: 42, paddingHorizontal: 12 },
  actionPressed: { transform: [{ scale: 0.95 }] },
  actionLabel: { fontFamily: font, fontWeight: '700', fontSize: 13, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.text },
  actionHint: { fontFamily: font, fontWeight: '500', fontSize: 10, color: colors.textMute, marginTop: 2 },
  backdrop: { ...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(4, 6, 14, 0.6)', alignItems: 'center', justifyContent: 'center' },
  menu: { ...panel, width: 420, padding: 28, borderRadius: 20 },
  menuEyebrow: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2.4, color: colors.gold },
  menuTitle: { fontFamily: font, fontWeight: '700', fontSize: 34, color: colors.text, marginTop: 6 },
  menuBody: { fontFamily: font, fontWeight: '400', fontSize: 14, lineHeight: 21, color: colors.textDim, marginTop: 10 },
  menuButtons: { flexDirection: 'row', gap: 10, marginTop: 22 },
});

// Started on the main surface by AppRegistry itself, unless the host picks
// what the main surface shows (the Three.js page renders App into a texture
// surface and leaves the main one empty).
AppRegistry.registerComponent('App', () => App);
