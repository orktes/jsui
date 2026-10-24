// Host textures in the Godot demo (cinema/cinema.tscn, main.gd's self-test):
// pictures Godot makes, shown inside the UI with <HostTexture name="..."/>.
// JSUI plays nothing itself: Godot decodes the video (VideoStreamPlayer,
// Ogg Theora) and renders the 3D preview (a SubViewport); cinema.gd hands
// their textures to the view with JsuiView.set_texture(name, texture, live),
// and the UI lays them out, clips them to rounded corners and composites them
// with everything else. Per frame no JS runs for them: the runtime repaints
// just the views showing a texture whose contents changed.
//
//   Godot -> UI   set_texture("tv", video.get_video_texture(), true)
//                 set_texture("preview", subviewport.get_texture(), true)
//                 set_state("cinema", {...})          playing / muted / hasVideo
//                 set_buffer("cinema.progress", [t])  the progress bar, no JS per frame
//   UI -> Godot   Host.postMessage("cinema:toggle" | "cinema:restart" | "cinema:mute")
//                 Host.postMessage("preview:color", {color}) / ("preview:spin")
import React, { useEffect, useState } from 'react';
import {
  Animated,
  Host,
  HostTexture,
  Pressable,
  StyleSheet,
  Text,
  View,
  useHostBufferValue,
  useHostState,
  useHostTexture,
} from 'react-native';
import { colors, font, panel } from '../../../../examples/theme.js';

// The film's credit, shown with the video (CC BY 3.0 asks for attribution).
const CREDIT = 'Big Buck Bunny (trailer) · © 2008 Blender Foundation · peach.blender.org · CC BY 3.0';
const SWATCHES = ['#f2c35b', '#4fd1ff', '#ff4d6a', '#4ade80', '#b26bff'];

function Button({ label, onPress, active }) {
  return (
    <Pressable
      onPress={onPress}
      style={({ pressed, hovered, focused }) => [
        styles.button,
        (hovered || focused) && styles.buttonHover,
        active && styles.buttonActive,
        pressed && styles.buttonPressed,
      ]}
    >
      <Text style={styles.buttonText}>{label}</Text>
    </Pressable>
  );
}

// The playback position from frame data (JsuiView.set_buffer): the fill moves
// natively, the component does not re-render while the video plays.
function Progress() {
  const [width, setWidth] = useState(0);
  const t = useHostBufferValue('cinema.progress', 0);
  const x = t.interpolate({ inputRange: [0, 1], outputRange: [-width, 0], extrapolate: 'clamp' });
  return (
    <View style={styles.track} onLayout={(e) => setWidth(e.nativeEvent.layout.width)}>
      <Animated.View style={[styles.fill, { transform: [{ translateX: x }] }]} />
    </View>
  );
}

function Tv({ cinema }) {
  const tv = useHostTexture('tv');
  if (!cinema.hasVideo) {
    return (
      <View style={[styles.screen, styles.placeholder]}>
        <Text style={styles.placeholderTitle}>NO SIGNAL</Text>
        <Text style={styles.placeholderText}>
          Run integrations/godot/demo/cinema/fetch-video.sh to download the Big Buck Bunny trailer (Ogg Theora, 4 MB,
          CC BY 3.0), then start this scene again.
        </Text>
      </View>
    );
  }
  return (
    <View style={styles.screen}>
      {/* The video's own aspect ratio, letterboxed in the screen. */}
      <HostTexture name="tv" resizeMode="contain" style={styles.fill100} />
      {!cinema.playing && (
        <View pointerEvents="none" style={styles.pausedBadge}>
          <Text style={styles.pausedText}>PAUSED</Text>
        </View>
      )}
      <Text style={styles.sizeTag}>{tv ? `${tv.width}×${tv.height}` : '…'}</Text>
    </View>
  );
}

export function Cinema() {
  const cinema = useHostState('cinema', { hasVideo: false, playing: false, muted: false, spinning: true, color: SWATCHES[0] });
  const preview = useHostTexture('preview');
  useEffect(() => Host.postMessage('cinema:ready', {}), []);
  return (
    <View style={styles.root}>
      <View style={styles.header}>
        <Text style={styles.kicker}>JSUI · HOST TEXTURES</Text>
        <Text style={styles.title}>Godot draws the pictures, JSUI frames them</Text>
      </View>
      <View style={styles.row}>
        <View style={[styles.panel, styles.tvPanel]}>
          <Text style={styles.label}>IN-GAME TV · VideoStreamPlayer</Text>
          <Tv cinema={cinema} />
          <View style={styles.controls}>
            <Button label={cinema.playing ? 'Pause' : 'Play'} onPress={() => Host.postMessage('cinema:toggle', {})} />
            <Button label="Restart" onPress={() => Host.postMessage('cinema:restart', {})} />
            <Button label={cinema.muted ? 'Unmute' : 'Mute'} active={cinema.muted} onPress={() => Host.postMessage('cinema:mute', {})} />
            <Progress />
          </View>
          <Text style={styles.credit}>{CREDIT}</Text>
        </View>
        <View style={[styles.panel, styles.previewPanel]}>
          <Text style={styles.label}>CHARACTER PREVIEW · SubViewport</Text>
          <View style={styles.previewFrame}>
            <HostTexture name="preview" resizeMode="cover" style={styles.fill100} />
          </View>
          <View style={styles.controls}>
            {SWATCHES.map((c) => (
              <Pressable
                key={c}
                onPress={() => Host.postMessage('preview:color', { color: c })}
                style={({ hovered }) => [styles.swatch, { backgroundColor: c }, (hovered || cinema.color === c) && styles.swatchOn]}
              />
            ))}
            <View style={{ flex: 1 }} />
            <Button label={cinema.spinning ? 'Stop' : 'Spin'} onPress={() => Host.postMessage('preview:spin', {})} />
          </View>
          <Text style={styles.credit}>{preview ? `${preview.width}×${preview.height} px, rendered by Godot every frame` : 'waiting for the viewport…'}</Text>
        </View>
      </View>
    </View>
  );
}

// main.gd's self-test: one host texture at a fixed size, stretched over it,
// and what useHostTexture reports for it.
export function TextureProbe({ name = 'probe', width = 64, height = 32 }) {
  const size = useHostTexture(name);
  useEffect(() => Host.postMessage('probe:size', { name, size }), [name, size]);
  return (
    <View style={{ width, height }}>
      <HostTexture name={name} resizeMode="stretch" style={{ width, height }} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, padding: 40, gap: 24, backgroundColor: colors.bg0 },
  header: { gap: 6 },
  kicker: { fontFamily: font, fontWeight: '700', fontSize: 12, letterSpacing: 3, color: colors.gold },
  title: { fontFamily: font, fontWeight: '700', fontSize: 28, color: colors.text },
  row: { flex: 1, flexDirection: 'row', gap: 24 },
  panel: { ...panel, padding: 18, gap: 14 },
  tvPanel: { flex: 1.7 },
  previewPanel: { flex: 1 },
  label: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.6, color: colors.textDim },
  screen: { flex: 1, borderRadius: 12, overflow: 'hidden', backgroundColor: '#000', borderWidth: 1, borderColor: colors.lineStrong },
  fill100: { width: '100%', height: '100%' },
  placeholder: { alignItems: 'center', justifyContent: 'center', padding: 32, gap: 12 },
  placeholderTitle: { fontFamily: font, fontWeight: '700', fontSize: 22, letterSpacing: 6, color: colors.textMute },
  placeholderText: { fontFamily: font, fontSize: 14, lineHeight: 21, color: colors.textDim, textAlign: 'center', maxWidth: 460 },
  pausedBadge: {
    position: 'absolute',
    top: 14,
    left: 14,
    paddingHorizontal: 10,
    height: 24,
    borderRadius: 12,
    justifyContent: 'center',
    backgroundColor: 'rgba(0, 0, 0, 0.6)',
  },
  pausedText: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2, color: colors.text },
  sizeTag: { position: 'absolute', right: 12, bottom: 10, fontFamily: font, fontSize: 11, color: 'rgba(255,255,255,0.55)' },
  previewFrame: {
    flex: 1,
    borderRadius: 120,
    overflow: 'hidden',
    borderWidth: 2,
    borderColor: 'rgba(242, 195, 91, 0.5)',
    boxShadow: '0 0 30px rgba(242, 195, 91, 0.25)',
    backgroundColor: colors.bg1,
  },
  controls: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  button: {
    height: 32,
    paddingHorizontal: 16,
    borderRadius: 16,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 'rgba(148, 170, 255, 0.12)',
    borderWidth: 1,
    borderColor: 'rgba(148, 170, 255, 0.25)',
  },
  buttonHover: { borderColor: 'rgba(242, 195, 91, 0.85)', backgroundColor: 'rgba(242, 195, 91, 0.18)' },
  buttonActive: { backgroundColor: 'rgba(255, 77, 106, 0.22)' },
  buttonPressed: { transform: [{ scale: 0.94 }] },
  buttonText: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.text },
  track: { flex: 1, height: 6, borderRadius: 3, overflow: 'hidden', backgroundColor: 'rgba(148, 170, 255, 0.16)' },
  fill: { width: '100%', height: '100%', borderRadius: 3, backgroundColor: colors.gold },
  swatch: { width: 26, height: 26, borderRadius: 13, borderWidth: 2, borderColor: 'transparent' },
  swatchOn: { borderColor: colors.text },
  credit: { fontFamily: font, fontSize: 11, color: colors.textMute },
});
