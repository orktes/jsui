// The UI of the Three.js example (site/three/): two JSUI surfaces that the
// page renders into GPUTextures of Three.js' own WebGPU device
// (createTextureSurface) and puts in the scene:
//
//   Console  a terminal on a screen in the 3D room. The page raycasts the
//            mouse onto it and forwards the events at the hit's UV. Its
//            "drone cam" is a Three.js render target shown with
//            <HostTexture>: the game's texture sampled in place, no copy.
//   Hud      a corner overlay the page composites in its own final pass.
//
// Messages to the page: 'core' { color }, 'spin' { speed }, 'pulse' { on }.
// Shared state from the page: 'scene' { fps, drawCalls, triangles }.
import React, { useState } from 'react';
import { AppRegistry, Host, HostTexture, Pressable, StyleSheet, Text, View, useHostState } from 'react-native';

const C = {
  bg: '#060b16',
  panel: 'rgba(10, 18, 34, 0.94)',
  line: 'rgba(120, 200, 255, 0.22)',
  text: '#e8f3ff',
  dim: '#8ea4c4',
  mute: '#56698a',
  cyan: '#4fd1ff',
  gold: '#f2c35b',
};
const CORES = [
  { name: 'Ion', color: '#4fd1ff' },
  { name: 'Ember', color: '#ff7a3c' },
  { name: 'Plasma', color: '#b26bff' },
  { name: 'Verdant', color: '#4ade80' },
];
const SPEEDS = [0, 0.4, 1, 2, 4];

function Button({ label, active, accent = C.cyan, onPress, style }) {
  return (
    <Pressable onPress={onPress} style={({ pressed, hovered }) => [
      s.button,
      active && { borderColor: accent, backgroundColor: accent + '26' },
      hovered && !active && { backgroundColor: 'rgba(120, 200, 255, 0.10)' },
      pressed && { transform: [{ scale: 0.96 }] },
      style,
    ]}>
      <Text style={[s.buttonText, active && { color: accent }]}>{label}</Text>
    </Pressable>
  );
}

function Console() {
  const [core, setCore] = useState(0);
  const [speed, setSpeed] = useState(2);
  const [pulse, setPulse] = useState(true);
  const scene = useHostState('scene', { fps: 0, drawCalls: 0, triangles: 0 });
  const pick = (i) => {
    setCore(i);
    Host.postMessage('core', { color: CORES[i].color });
  };
  const spin = (i) => {
    setSpeed(i);
    Host.postMessage('spin', { speed: SPEEDS[i] });
  };
  const accent = CORES[core].color;
  return (
    <View style={s.console}>
      <View style={s.header}>
        <View style={[s.led, { backgroundColor: accent, boxShadow: `0 0 12px ${accent}` }]} />
        <Text style={s.title}>REACTOR CONSOLE</Text>
        <Text style={s.kicker}>JSUI · in-world</Text>
      </View>
      <View style={s.body}>
        <View style={s.feed}>
          <HostTexture name="drone-cam" resizeMode="cover" style={s.feedPicture} />
          <View style={s.feedLabel}>
            <View style={s.rec} />
            <Text style={s.feedText}>DRONE CAM · Three.js render target</Text>
          </View>
        </View>
        <View style={s.controls}>
          <Text style={s.label}>Core</Text>
          <View style={s.row}>
            {CORES.map((c, i) => (
              <Pressable key={c.name} onPress={() => pick(i)} style={({ hovered, pressed }) => [
                s.swatch, { backgroundColor: c.color },
                i === core && { borderColor: '#ffffff', boxShadow: `0 0 16px ${c.color}` },
                hovered && i !== core && { opacity: 0.85 },
                pressed && { transform: [{ scale: 0.92 }] },
              ]} />
            ))}
          </View>
          <Text style={[s.value, { color: accent }]}>{CORES[core].name}</Text>
          <Text style={[s.label, { marginTop: 14 }]}>Spin</Text>
          <View style={s.row}>
            {SPEEDS.map((v, i) => (
              <Button key={v} label={v === 0 ? 'Stop' : `${v}x`} active={i === speed} accent={accent}
                onPress={() => spin(i)} style={{ flex: 1 }} />
            ))}
          </View>
          <Text style={[s.label, { marginTop: 14 }]}>Field</Text>
          <Button label={pulse ? 'Pulse on' : 'Pulse off'} active={pulse} accent={accent}
            onPress={() => { setPulse(!pulse); Host.postMessage('pulse', { on: !pulse }); }} />
        </View>
      </View>
      <View style={s.footer}>
        <Text style={s.stat}>{scene.fps.toFixed(0)} FPS</Text>
        <Text style={s.stat}>{scene.drawCalls} draws</Text>
        <Text style={s.stat}>{(scene.triangles / 1000).toFixed(1)}k tris</Text>
        <Text style={[s.stat, { marginLeft: 'auto', color: C.mute }]}>one GPUDevice · zero copies</Text>
      </View>
    </View>
  );
}

function Hud() {
  const scene = useHostState('scene', { fps: 0 });
  return (
    <View style={s.hud}>
      <Text style={s.hudKicker}>SHARED GPUDEVICE</Text>
      <Text style={s.hudTitle}>Three.js scene + JSUI</Text>
      <Text style={s.hudBody}>The console and this HUD are JSUI surfaces rendered into Three.js textures. Click the screen in the room.</Text>
      <Text style={[s.hudBody, { color: C.cyan, marginTop: 6 }]}>{scene.fps.toFixed(0)} FPS</Text>
    </View>
  );
}

const s = StyleSheet.create({
  console: { flex: 1, backgroundColor: C.bg, borderRadius: 18, borderWidth: 2, borderColor: C.line, padding: 22 },
  header: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  led: { width: 10, height: 10, borderRadius: 5 },
  title: { fontFamily: 'Inter', fontWeight: '700', fontSize: 20, letterSpacing: 3, color: C.text },
  kicker: { fontFamily: 'Inter', fontWeight: '600', fontSize: 11, letterSpacing: 1.4, color: C.mute, marginLeft: 'auto',
    textTransform: 'uppercase' },
  body: { flex: 1, flexDirection: 'row', gap: 18, marginTop: 18 },
  feed: { flex: 1.25, borderRadius: 12, overflow: 'hidden', borderWidth: 1, borderColor: C.line, backgroundColor: '#000' },
  feedPicture: { flex: 1 },
  feedLabel: { position: 'absolute', left: 10, top: 10, flexDirection: 'row', alignItems: 'center', gap: 6,
    backgroundColor: 'rgba(0, 0, 0, 0.55)', paddingHorizontal: 8, paddingVertical: 4, borderRadius: 6 },
  rec: { width: 7, height: 7, borderRadius: 4, backgroundColor: '#ff4d6a' },
  feedText: { fontFamily: 'Inter', fontWeight: '600', fontSize: 10, letterSpacing: 0.8, color: C.text },
  controls: { flex: 1 },
  label: { fontFamily: 'Inter', fontWeight: '600', fontSize: 11, letterSpacing: 1.6, textTransform: 'uppercase',
    color: C.dim, marginBottom: 8 },
  row: { flexDirection: 'row', gap: 6 },
  swatch: { width: 40, height: 40, borderRadius: 10, borderWidth: 2, borderColor: 'transparent' },
  value: { fontFamily: 'Inter', fontWeight: '700', fontSize: 14, marginTop: 8 },
  button: { height: 34, paddingHorizontal: 4, borderRadius: 8, borderWidth: 1, borderColor: C.line,
    alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(120, 200, 255, 0.04)' },
  buttonText: { fontFamily: 'Inter', fontWeight: '700', fontSize: 12, color: C.text },
  footer: { flexDirection: 'row', gap: 16, marginTop: 16, paddingTop: 12, borderTopWidth: 1, borderColor: C.line },
  stat: { fontFamily: 'Inter', fontWeight: '600', fontSize: 12, color: C.dim },
  hud: { flex: 1, backgroundColor: C.panel, borderRadius: 14, borderWidth: 1, borderColor: C.line, padding: 16 },
  hudKicker: { fontFamily: 'Inter', fontWeight: '700', fontSize: 10, letterSpacing: 1.6, color: C.gold },
  hudTitle: { fontFamily: 'Inter', fontWeight: '700', fontSize: 17, color: C.text, marginTop: 4 },
  hudBody: { fontFamily: 'Inter', fontSize: 12, lineHeight: 17, color: C.dim, marginTop: 6 },
});

// The main surface is not shown (the page makes only texture surfaces).
AppRegistry.registerComponent('Main', () => () => null);
AppRegistry.registerComponent('Console', () => Console);
AppRegistry.registerComponent('Hud', () => Hud);
