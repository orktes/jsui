// Cinematic (interactive). Feature spotlight: host textures.
//
// A game-style trailer player. JSUI does not play video: the screen asks its
// host for one with a host message naming the sources (below), and the host
// decodes it into a host texture: in the browser the page's <video>
// (jsui.bindVideo('trailer', video), web/jsui-textures.js), natively
// jsui-viewer with AVFoundation (tools/jsui-viewer/video.mm). This screen only
// shows that texture by name, five times over: the player, a blurred
// backdrop, a picture-in-picture view, a close-up and a night-vision feed. Each
// video frame is uploaded once and every view samples it in place; no JS
// runs per frame.
//
// Everything around the picture is JSUI: the frame, the custom progress bar
// (driven by frame data, Animated.hostBuffer, no re-render per frame), the
// play / pause and mute buttons, the scene list. Commands go to the host with
// Host.postMessage('video', { id: 'cinematic', cmd }); it answers through
// HostState 'cinematic' ({ status, muted, duration, backend, quality }) and
// HostBuffer 'cinematic.time' ([fraction, buffered fraction, seconds]).
//
// Without a network (or before the first play when autoplay is off) the
// screen shows its own poster art instead of the texture.
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  Animated,
  Easing,
  Host,
  HostTexture,
  ImageBackground,
  PanResponder,
  Pressable,
  StyleSheet,
  Text,
  View,
  useHostBufferSelector,
  useHostBufferValue,
  useHostState,
  useHostTexture,
} from 'react-native';
import { colors, font } from '../../../examples/theme.js';

export const feature = {
  title: 'Host textures',
  points: [
    'The page plays a <video> and hands each frame to the runtime with jsui.bindVideo(): one upload per frame, into a GPU texture on WebGPU / WebGL',
    '<HostTexture name="trailer"> shows it like an Image: rounded corners, resizeMode, transforms, filters. Five views here sample the same texture',
    'The progress bar follows frame data (Animated.hostBuffer) and play / pause / seek / mute are host messages: no JS runs per video frame',
    'JSUI never decodes anything: a 3D viewport, a webcam or a canvas works the same way',
  ],
  keys: ['Space / K / A: play / pause', '← → / D-pad: seek 10 s', 'M / X: mute', '1–5: jump to a scene', 'Click the picture: unmute'],
};

const TEXTURE = 'trailer';
const DURATION = 596.5; // until the page reports the real one
const SCENES = [
  { t: 0, title: 'Morning in the meadow' },
  { t: 112, title: 'Uninvited guests' },
  { t: 262, title: 'The last straw' },
  { t: 372, title: 'Payback' },
  { t: 490, title: 'Curtain call' },
];
const PLAYER_W = 832;
const PLAYER_H = 468;
const TRACK_W = 470;
const BACKEND = {
  webgpu: 'WebGPU · sampled in place',
  gl: 'WebGL · GPU texture',
  pixels: 'CPU pixels',
  metal: 'Metal · sampled in place',
};
const POSTER = require('../../../examples/assets/bg-menu.jpg');

// The trailer, as the host is asked to play it: the first source it can play
// (the browser: WebM / VP9; jsui-viewer on macOS, AVFoundation: HLS / H.264).
// Big Buck Bunny (c) 2008 Blender Foundation, peach.blender.org, CC BY 3.0,
// from Wikimedia Commons (CORS-enabled, so GPU uploads may read it) and
// Mux's public HLS test stream.
const WIKIMEDIA = 'https://upload.wikimedia.org/wikipedia/commons/transcoded/c/c0/Big_Buck_Bunny_4K.webm/Big_Buck_Bunny_4K.webm.';
const SOURCES = [
  { url: WIKIMEDIA + '1080p.vp9.webm', type: 'video/webm; codecs="vp9"', height: 1080, quality: '1080p VP9' },
  { url: WIKIMEDIA + '480p.vp9.webm', type: 'video/webm; codecs="vp9"', height: 480, quality: '480p VP9' },
  { url: 'https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8', type: 'application/vnd.apple.mpegurl', height: 1080, quality: 'HLS H.264 · 1080p' },
];
const POSTER_URL = 'https://upload.wikimedia.org/wikipedia/commons/thumb/c/c0/Big_Buck_Bunny_4K.webm/1280px--Big_Buck_Bunny_4K.webm.jpg';
// The host's video protocol (site/index.html on the web, tools/jsui-viewer/
// video.mm natively): 'video' messages for this id; it answers through
// HostState 'cinematic' and HostBuffer 'cinematic.time'.
const send = (cmd, extra) => Host.postMessage('video', { id: 'cinematic', cmd, ...extra });
const pad2 = (n) => String(n).padStart(2, '0');
const fmt = (s) => {
  s = Math.max(0, Math.floor(s || 0));
  return `${pad2(Math.floor(s / 60))}:${pad2(s % 60)}`;
};
const IDLE = { status: 'idle', muted: true, duration: 0, backend: null, quality: null };

export default function Cinematic({ active }) {
  const st = useHostState('cinematic', IDLE) || IDLE;
  const tex = useHostTexture(TEXTURE);
  const secs = useHostBufferSelector('cinematic.time', (b) => Math.floor(b[2] || 0), 0);
  const duration = st.duration > 0 ? st.duration : DURATION;
  const playing = st.status === 'playing';
  const scene = SCENES.reduce((cur, s, i) => (secs >= s.t ? i : cur), 0);

  // The page opens the trailer while the screen is shown (and pauses it after).
  useEffect(() => {
    if (!active) return undefined;
    send('open', { texture: TEXTURE, sources: SOURCES, poster: POSTER_URL });
    return () => send('close');
  }, [active]);

  const intro = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    Animated.timing(intro, { toValue: 1, duration: 900, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, []);

  const toggle = useCallback(() => send(playing ? 'pause' : 'play'), [playing]);
  const seekBy = (d) => send('seek', { to: Math.max(0, Math.min(duration - 1, secs + d)) });
  const onKeyDown = (e) => {
    const k = e.key;
    let used = true;
    if (k === ' ' || k === 'k' || k === 'K' || k === 'Enter') toggle();
    else if (k === 'ArrowLeft' || k === 'ArrowRight') seekBy(k === 'ArrowLeft' ? -10 : 10);
    else if (k === 'm' || k === 'M') send('mute', { muted: !st.muted });
    else if (k >= '1' && k <= String(SCENES.length)) send('seek', { to: SCENES[+k - 1].t, play: true });
    else used = false;
    if (used) {
      e.preventDefault();
      e.stopPropagation();
    }
  };
  const onPad = (e) => {
    if (!e.pressed) return;
    const b = e.buttonIndex;
    let used = true;
    if (b === 0) toggle();
    else if (b === 2) send('mute', { muted: !st.muted });
    else if (b === 14 || b === 15) seekBy(b === 14 ? -10 : 10);
    else used = false;
    if (used) {
      e.preventDefault();
      e.stopPropagation();
    }
  };

  const rise = (from, d = 0) => ({
    opacity: intro.interpolate({ inputRange: [d, 1], outputRange: [0, 1], extrapolate: 'clamp' }),
    transform: [{ translateY: intro.interpolate({ inputRange: [0, 1], outputRange: [from, 0] }) }],
  });
  const backend = st.backend ? BACKEND[st.backend] || st.backend : null;

  return (
    <View focusable autoFocus={active} onKeyDown={onKeyDown} onGamepadButton={onPad} style={styles.root}>
      {/* Backdrop: the same texture, blurred (a still on the CPU raster, where blurring every frame costs too much). */}
      {tex ? (
        <HostTexture
          name={TEXTURE}
          resizeMode="cover"
          style={[StyleSheet.absoluteFill, { opacity: 0.42, transform: [{ scale: 1.15 }] }, st.backend !== 'pixels' && { filter: 'blur(36px) saturate(1.4)' }]}
        />
      ) : (
        <ImageBackground source={POSTER} resizeMode="cover" style={[StyleSheet.absoluteFill, { opacity: 0.3 }]} />
      )}
      <View pointerEvents="none" style={styles.grade} />
      <View pointerEvents="none" style={styles.vignette} />

      <Animated.View style={[styles.header, rise(-10)]}>
        <View style={{ gap: 6 }}>
          <View style={styles.eyebrowRow}>
            <View style={styles.diamond} />
            <Text style={styles.eyebrow}>Cinematic · Prologue</Text>
          </View>
          <Text style={styles.title}>Big Buck Bunny</Text>
          <Text style={styles.subtitle}>A short film from the Blender Institute · 2008 · 9 min 56 s</Text>
        </View>
        <View style={styles.headerRight}>
          <Pill label="Host texture" value={backend || 'waiting for the first frame'} accent={colors.cyan} />
          <Pill label="Source" value={tex ? `${st.quality || 'video'} · ${tex.width}×${tex.height}` : '<video> on the page'} accent={colors.gold} />
        </View>
      </Animated.View>

      <Animated.View style={[styles.playerWrap, rise(16, 0.1)]}>
        <Player st={st} tex={tex} secs={secs} duration={duration} toggle={toggle} active={active} />
      </Animated.View>

      <Animated.View style={[styles.scenes, rise(20, 0.25)]}>
        {SCENES.map((s, i) => (
          <SceneChip key={s.t} i={i} s={s} current={i === scene} next={SCENES[i + 1] ? SCENES[i + 1].t : duration} secs={secs} />
        ))}
      </Animated.View>

      <Animated.View style={[styles.side, rise(24, 0.2)]}>
        <Feeds tex={tex} live={playing} />
        <View style={styles.info}>
          <Text style={styles.infoTitle}>One texture, five views</Text>
          <Text style={styles.infoBody}>
            The page uploads each video frame once. The player, backdrop and the three feeds all sample that one texture: no JS runs per frame.
          </Text>
          <View style={styles.statRow}>
            <Stat k="Views" v="5" />
            <Stat k="Uploads / frame" v="1" />
            <Stat k="JS / frame" v="0" />
          </View>
        </View>
        <View style={styles.credit}>
          <Text style={styles.creditTitle}>Big Buck Bunny</Text>
          <Text style={styles.creditText}>© 2008 Blender Foundation · CC BY 3.0</Text>
        </View>
      </Animated.View>
    </View>
  );
}

// --- the player ------------------------------------------------------------------------

const Player = memo(function Player({ st, tex, secs, duration, toggle, active }) {
  const status = st.status;
  const playing = status === 'playing';
  const loading = status === 'loading';
  const failed = status === 'error';
  const onPicture = () => {
    // The first click on a muted autoplaying trailer turns the sound on.
    if (playing && st.muted) send('mute', { muted: false });
    else toggle();
  };
  return (
    <View style={styles.player}>
      {tex ? (
        <HostTexture name={TEXTURE} resizeMode="cover" style={StyleSheet.absoluteFill} />
      ) : (
        <ImageBackground source={POSTER} resizeMode="cover" style={StyleSheet.absoluteFill}>
          <View style={styles.posterShade} />
        </ImageBackground>
      )}
      <Pressable onPress={onPicture} focusable={false} style={[StyleSheet.absoluteFill, { cursor: 'pointer' }]}>
        <View pointerEvents="none" style={styles.topShade} />
        <View pointerEvents="none" style={styles.bottomShade} />
      </Pressable>

      <View pointerEvents="none" style={styles.tagRow}>
        <LiveTag status={status} />
      </View>
      {playing && st.muted ? (
        <Pressable onPress={() => send('mute', { muted: false })} focusable={false} style={styles.unmuteWrap}>
          {({ hovered }) => (
            <View style={[styles.unmute, hovered && styles.unmuteHot]}>
              <SpeakerIcon muted />
              <Text style={styles.unmuteText}>Sound off · click to unmute</Text>
            </View>
          )}
        </Pressable>
      ) : null}

      {!playing ? (
        <View pointerEvents="box-none" style={styles.center}>
          {loading ? (
            <Spinner active={active} />
          ) : (
            <BigPlay onPress={toggle} label={failed ? 'Retry' : status === 'ended' ? 'Watch again' : tex ? 'Resume' : 'Play trailer'} />
          )}
          {failed ? (
            <View style={styles.errorBox}>
              <Text style={styles.errorTitle}>Trailer unavailable</Text>
              <Text style={styles.errorText}>
                It streams from the internet, which this host could not reach or play. The poster stays until it can.
              </Text>
            </View>
          ) : null}
        </View>
      ) : null}

      <View style={styles.controls}>
        <PlayButton playing={playing} onPress={toggle} />
        <Text style={styles.time}>
          {fmt(secs)}
          <Text style={styles.timeDim}> / {fmt(duration)}</Text>
        </Text>
        <Scrubber duration={duration} />
        <Pressable onPress={() => send('mute', { muted: !st.muted })} focusable={false} style={{ cursor: 'pointer' }}>
          {({ hovered }) => (
            <View style={[styles.iconButton, hovered && styles.iconButtonHot]}>
              <SpeakerIcon muted={st.muted} />
            </View>
          )}
        </Pressable>
        <View style={styles.hd}>
          <Text style={styles.hdText}>{st.quality && /1080|720/.test(st.quality) ? 'HD' : 'SD'}</Text>
        </View>
      </View>
    </View>
  );
});

function LiveTag({ status }) {
  const pulse = useRef(new Animated.Value(0)).current;
  const live = status === 'playing';
  useEffect(() => {
    if (!live) return undefined;
    const a = Animated.loop(
      Animated.sequence([
        Animated.timing(pulse, { toValue: 1, duration: 700, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
        Animated.timing(pulse, { toValue: 0, duration: 700, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
      ]),
    );
    a.start();
    return () => a.stop();
  }, [live]);
  const label = { playing: 'Live texture', paused: 'Paused', loading: 'Buffering', ended: 'Ended', error: 'Offline' }[status] || 'Ready';
  const color = live ? colors.red : status === 'error' ? colors.orange : colors.textDim;
  return (
    <View style={styles.tag}>
      <Animated.View style={[styles.tagDot, { backgroundColor: color, opacity: live ? pulse.interpolate({ inputRange: [0, 1], outputRange: [1, 0.35] }) : 1 }]} />
      <Text style={[styles.tagText, { color: live ? colors.text : colors.textDim }]}>{label}</Text>
    </View>
  );
}

function BigPlay({ onPress, label }) {
  return (
    <Pressable onPress={onPress} focusable={false} style={{ cursor: 'pointer', alignItems: 'center', gap: 14 }}>
      {({ hovered, pressed }) => (
        <>
          <View style={[styles.bigPlay, hovered && styles.bigPlayHot, pressed && { transform: [{ scale: 0.94 }] }]}>
            <Text style={styles.bigPlayGlyph}>▶</Text>
          </View>
          <Text style={styles.bigPlayLabel}>{label}</Text>
        </>
      )}
    </Pressable>
  );
}

function PlayButton({ playing, onPress }) {
  return (
    <Pressable onPress={onPress} focusable={false} style={{ cursor: 'pointer' }}>
      {({ hovered, pressed }) => (
        <View style={[styles.play, hovered && styles.playHot, pressed && { transform: [{ scale: 0.92 }] }]}>
          {playing ? (
            <View style={styles.pauseGlyph}>
              <View style={styles.pauseBar} />
              <View style={styles.pauseBar} />
            </View>
          ) : (
            <Text style={styles.playGlyph}>▶</Text>
          )}
        </View>
      )}
    </Pressable>
  );
}

function SpeakerIcon({ muted }) {
  return (
    <View style={styles.speaker}>
      <View style={styles.speakerBox} />
      <View style={styles.speakerCone} />
      {muted ? (
        <View style={styles.speakerSlash} />
      ) : (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 2, marginLeft: 3 }}>
          <View style={[styles.wave, { height: 6 }]} />
          <View style={[styles.wave, { height: 10 }]} />
        </View>
      )}
    </View>
  );
}

function Spinner({ active }) {
  const spin = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    if (!active) return undefined;
    const a = Animated.loop(Animated.timing(spin, { toValue: 1, duration: 900, easing: Easing.linear, useNativeDriver: true }));
    a.start();
    return () => a.stop();
  }, [active]);
  return (
    <View style={{ alignItems: 'center', gap: 16 }}>
      <Animated.View style={[styles.spinner, { transform: [{ rotate: spin.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '360deg'] }) }] }]} />
      <Text style={styles.bigPlayLabel}>Streaming the trailer…</Text>
    </View>
  );
}

// --- progress bar: follows frame data, scrubs with a drag ---------------------------------

function Scrubber({ duration }) {
  const progress = useHostBufferValue('cinematic.time', 0);
  const buffered = useHostBufferValue('cinematic.time', 1);
  const drag = useRef(new Animated.Value(0)).current;
  const [dragging, setDragging] = useState(false);
  const [hover, setHover] = useState(false);
  const frac = (x) => Math.max(0, Math.min(1, x / TRACK_W));
  const pan = useMemo(
    () =>
      PanResponder.create({
        onStartShouldSetPanResponder: () => true,
        onMoveShouldSetPanResponder: () => true,
        onPanResponderGrant: (e) => {
          drag.setValue(frac(e.nativeEvent.locationX));
          setDragging(true);
        },
        onPanResponderMove: (e) => drag.setValue(frac(e.nativeEvent.locationX)),
        onPanResponderRelease: (e) => {
          send('seek', { fraction: frac(e.nativeEvent.locationX) });
          setDragging(false);
        },
        onPanResponderTerminate: () => setDragging(false),
      }),
    [],
  );
  const shown = dragging ? drag : progress;
  const fill = (v) => ({ transform: [{ translateX: v.interpolate({ inputRange: [0, 1], outputRange: [-TRACK_W, 0], extrapolate: 'clamp' }) }] });
  const big = dragging || hover;
  return (
    <View style={styles.scrubHit} onPointerEnter={() => setHover(true)} onPointerLeave={() => setHover(false)} {...pan.panHandlers}>
      <View pointerEvents="none" style={[styles.track, big && styles.trackBig]}>
        <Animated.View style={[styles.buffered, fill(buffered)]} />
        <Animated.View style={[styles.progress, fill(shown)]} />
        {SCENES.slice(1).map((s) => (
          <View key={s.t} style={[styles.tick, { left: Math.round((s.t / duration) * TRACK_W) }]} />
        ))}
      </View>
      <Animated.View
        pointerEvents="none"
        style={[
          styles.thumb,
          big && styles.thumbBig,
          { transform: [{ translateX: shown.interpolate({ inputRange: [0, 1], outputRange: [0, TRACK_W], extrapolate: 'clamp' }) }] },
        ]}
      />
    </View>
  );
}

// --- scenes --------------------------------------------------------------------------------

const SceneChip = memo(function SceneChip({ s, i, current, next, secs }) {
  const p = current ? Math.max(0, Math.min(1, (secs - s.t) / Math.max(1, next - s.t))) : 0;
  return (
    <Pressable onPress={() => send('seek', { to: s.t, play: true })} focusable={false} style={{ flex: 1, cursor: 'pointer' }}>
      {({ hovered, pressed }) => (
        <View style={[styles.chip, hovered && styles.chipHot, current && styles.chipCurrent, pressed && { transform: [{ scale: 0.97 }] }]}>
          <View style={styles.chipTop}>
            <Text style={[styles.chipNum, current && { color: colors.gold }]}>{pad2(i + 1)}</Text>
            <Text style={styles.chipTime}>{fmt(s.t)}</Text>
          </View>
          <Text style={[styles.chipTitle, (current || hovered) && { color: colors.text }]} numberOfLines={1}>
            {s.title}
          </Text>
          <View style={styles.chipBar}>
            <View style={[styles.chipFill, { width: `${Math.round(p * 1000) / 10}%` }]} />
          </View>
        </View>
      )}
    </Pressable>
  );
});

// --- the same texture as other feeds ----------------------------------------------------------

function Feeds({ tex, live }) {
  return (
    <View style={{ gap: 14 }}>
      <View style={{ gap: 8 }}>
        <FeedLabel title="Picture in picture" live={live} />
        <View style={styles.pip}>
          {tex ? <HostTexture name={TEXTURE} resizeMode="cover" style={StyleSheet.absoluteFill} /> : <NoSignal />}
          <View pointerEvents="none" style={styles.pipFrame} />
        </View>
      </View>
      <View style={{ flexDirection: 'row', gap: 10 }}>
        <View style={{ flex: 1, gap: 8 }}>
          <FeedLabel title="Close-up" />
          <View style={styles.feed}>
            {tex ? (
              <HostTexture name={TEXTURE} resizeMode="cover" style={[StyleSheet.absoluteFill, { transform: [{ scale: 2.4 }, { translateX: 8 }, { translateY: 6 }] }]} />
            ) : (
              <NoSignal />
            )}
            <View pointerEvents="none" style={styles.crosshairH} />
            <View pointerEvents="none" style={styles.crosshairV} />
          </View>
        </View>
        <View style={{ flex: 1, gap: 8 }}>
          <FeedLabel title="Night vision" />
          <View style={styles.feed}>
            {tex ? (
              <>
                <HostTexture name={TEXTURE} resizeMode="cover" style={[StyleSheet.absoluteFill, { filter: 'grayscale(1) contrast(1.6) brightness(0.9)' }]} />
                <View pointerEvents="none" style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(40, 255, 120, 0.16)' }]} />
                <View pointerEvents="none" style={styles.scanlines} />
              </>
            ) : (
              <NoSignal />
            )}
          </View>
        </View>
      </View>
    </View>
  );
}

function FeedLabel({ title, live }) {
  return (
    <View style={styles.feedLabel}>
      <Text style={styles.feedTitle}>{title}</Text>
      {live ? (
        <View style={styles.feedLive}>
          <View style={[styles.tagDot, { width: 6, height: 6, backgroundColor: colors.red }]} />
          <Text style={styles.feedLiveText}>Live</Text>
        </View>
      ) : null}
    </View>
  );
}

function NoSignal() {
  return (
    <View style={[StyleSheet.absoluteFill, styles.noSignal]}>
      <Text style={styles.noSignalText}>No signal</Text>
    </View>
  );
}

function Pill({ label, value, accent }) {
  return (
    <View style={[styles.pill, { borderColor: accent + '55' }]}>
      <Text style={[styles.pillLabel, { color: accent }]}>{label}</Text>
      <Text style={styles.pillValue} numberOfLines={1}>
        {value}
      </Text>
    </View>
  );
}

function Stat({ k, v }) {
  return (
    <View style={styles.stat}>
      <Text style={styles.statV}>{v}</Text>
      <Text style={styles.statK}>{k}</Text>
    </View>
  );
}

const glass = 'rgba(8, 12, 24, 0.62)';

const styles = StyleSheet.create({
  root: { width: 1280, height: 720, backgroundColor: '#04060c', overflow: 'hidden' },
  grade: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage:
      'linear-gradient(180deg, rgba(4, 6, 12, 0.72) 0%, rgba(4, 6, 12, 0.35) 38%, rgba(4, 6, 12, 0.9) 100%)',
  },
  vignette: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage: 'radial-gradient(ellipse at 35% 45%, rgba(4, 6, 12, 0) 40%, rgba(4, 6, 12, 0.75) 100%)',
  },

  header: { position: 'absolute', left: 48, right: 48, top: 26, flexDirection: 'row', alignItems: 'flex-end', justifyContent: 'space-between' },
  eyebrowRow: { flexDirection: 'row', alignItems: 'center', gap: 9 },
  diamond: { width: 8, height: 8, backgroundColor: colors.gold, transform: [{ rotate: '45deg' }], boxShadow: `0 0 10px ${colors.gold}` },
  eyebrow: { fontFamily: font, fontWeight: '700', fontSize: 12, letterSpacing: 3, textTransform: 'uppercase', color: colors.gold },
  title: { fontFamily: font, fontWeight: '700', fontSize: 38, letterSpacing: -0.5, color: colors.text, textShadowColor: 'rgba(0,0,0,0.6)', textShadowRadius: 18 },
  subtitle: { fontFamily: font, fontSize: 14, color: colors.textDim },
  headerRight: { flexDirection: 'row', gap: 10, paddingBottom: 4 },
  pill: { paddingHorizontal: 12, paddingVertical: 7, borderRadius: 10, borderWidth: 1, backgroundColor: glass, gap: 2, maxWidth: 250 },
  pillLabel: { fontFamily: font, fontWeight: '700', fontSize: 9.5, letterSpacing: 1.8, textTransform: 'uppercase' },
  pillValue: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },

  playerWrap: { position: 'absolute', left: 48, top: 116, width: PLAYER_W, height: PLAYER_H },
  player: {
    width: PLAYER_W,
    height: PLAYER_H,
    borderRadius: 22,
    overflow: 'hidden',
    backgroundColor: '#000',
    borderWidth: 1,
    borderColor: 'rgba(242, 195, 91, 0.38)',
    boxShadow: '0 30px 80px rgba(0, 0, 0, 0.65), 0 0 70px rgba(242, 195, 91, 0.10)',
  },
  posterShade: { ...StyleSheet.absoluteFillObject, backgroundColor: 'rgba(4, 6, 12, 0.45)' },
  topShade: {
    position: 'absolute', left: 0, right: 0, top: 0, height: 110,
    experimental_backgroundImage: 'linear-gradient(180deg, rgba(0,0,0,0.55) 0%, rgba(0,0,0,0) 100%)',
  },
  bottomShade: {
    position: 'absolute', left: 0, right: 0, bottom: 0, height: 150,
    experimental_backgroundImage: 'linear-gradient(0deg, rgba(0,0,0,0.82) 0%, rgba(0,0,0,0) 100%)',
  },
  tagRow: { position: 'absolute', left: 20, top: 18, flexDirection: 'row', gap: 8 },
  tag: { flexDirection: 'row', alignItems: 'center', gap: 7, paddingHorizontal: 10, height: 26, borderRadius: 13, backgroundColor: glass, borderWidth: 1, borderColor: 'rgba(255,255,255,0.10)' },
  tagDot: { width: 8, height: 8, borderRadius: 4 },
  tagText: { fontFamily: font, fontWeight: '700', fontSize: 10.5, letterSpacing: 1.6, textTransform: 'uppercase' },
  unmuteWrap: { position: 'absolute', right: 18, top: 16, cursor: 'pointer' },
  unmute: { flexDirection: 'row', alignItems: 'center', gap: 9, paddingHorizontal: 13, height: 32, borderRadius: 16, backgroundColor: 'rgba(14, 12, 6, 0.78)', borderWidth: 1, borderColor: 'rgba(242, 195, 91, 0.7)', boxShadow: '0 4px 14px rgba(0,0,0,0.5)' },
  unmuteHot: { backgroundColor: 'rgba(46, 36, 12, 0.9)' },
  unmuteText: { fontFamily: font, fontWeight: '600', fontSize: 12.5, color: colors.goldLight },

  center: { ...StyleSheet.absoluteFillObject, alignItems: 'center', justifyContent: 'center', gap: 22, paddingBottom: 30 },
  bigPlay: {
    width: 92, height: 92, borderRadius: 46, alignItems: 'center', justifyContent: 'center',
    backgroundColor: 'rgba(8, 12, 24, 0.55)', borderWidth: 2, borderColor: colors.gold,
    boxShadow: '0 0 0 8px rgba(242, 195, 91, 0.12), 0 0 40px rgba(242, 195, 91, 0.35)',
  },
  bigPlayHot: { backgroundColor: 'rgba(242, 195, 91, 0.22)', boxShadow: '0 0 0 12px rgba(242, 195, 91, 0.16), 0 0 60px rgba(242, 195, 91, 0.5)' },
  bigPlayGlyph: { fontFamily: font, fontSize: 34, color: colors.goldLight, marginLeft: 7, marginTop: -2 },
  bigPlayLabel: { fontFamily: font, fontWeight: '700', fontSize: 13, letterSpacing: 2.4, textTransform: 'uppercase', color: colors.text, textShadowColor: 'rgba(0,0,0,0.8)', textShadowRadius: 10 },
  spinner: { width: 56, height: 56, borderRadius: 28, borderWidth: 4, borderColor: 'rgba(255,255,255,0.12)', borderTopColor: colors.gold },
  errorBox: { maxWidth: 420, padding: 14, borderRadius: 12, backgroundColor: glass, borderWidth: 1, borderColor: 'rgba(255, 154, 60, 0.45)', gap: 4 },
  errorTitle: { fontFamily: font, fontWeight: '700', fontSize: 14, color: colors.orange, textAlign: 'center' },
  errorText: { fontFamily: font, fontSize: 12.5, lineHeight: 18, color: colors.textDim, textAlign: 'center' },

  controls: { position: 'absolute', left: 20, right: 20, bottom: 18, height: 44, flexDirection: 'row', alignItems: 'center', gap: 14 },
  play: { width: 44, height: 44, borderRadius: 22, alignItems: 'center', justifyContent: 'center', backgroundColor: colors.gold, boxShadow: '0 6px 18px rgba(242, 195, 91, 0.35)' },
  playHot: { backgroundColor: colors.goldLight },
  playGlyph: { fontFamily: font, fontSize: 17, color: '#1a1206', marginLeft: 3, marginTop: -1 },
  pauseGlyph: { flexDirection: 'row', gap: 5 },
  pauseBar: { width: 5, height: 16, borderRadius: 1.5, backgroundColor: '#1a1206' },
  time: { width: 112, fontFamily: 'JetBrains Mono', fontWeight: '700', fontSize: 13, color: colors.text },
  timeDim: { color: colors.textMute, fontWeight: '400' },
  scrubHit: { width: TRACK_W, height: 30, justifyContent: 'center', cursor: 'pointer' },
  track: { width: TRACK_W, height: 5, borderRadius: 3, backgroundColor: 'rgba(255,255,255,0.14)', overflow: 'hidden' },
  trackBig: { height: 8, borderRadius: 4 },
  buffered: { position: 'absolute', left: 0, top: 0, bottom: 0, width: TRACK_W, backgroundColor: 'rgba(255,255,255,0.22)' },
  progress: {
    position: 'absolute', left: 0, top: 0, bottom: 0, width: TRACK_W,
    experimental_backgroundImage: `linear-gradient(90deg, ${colors.goldDeep} 0%, ${colors.gold} 70%, ${colors.goldLight} 100%)`,
  },
  tick: { position: 'absolute', top: 0, bottom: 0, width: 2, marginLeft: -1, backgroundColor: 'rgba(4, 6, 12, 0.85)' },
  thumb: {
    position: 'absolute', left: -7, top: 8, width: 14, height: 14, borderRadius: 7,
    backgroundColor: colors.goldLight, boxShadow: '0 0 0 3px rgba(242, 195, 91, 0.25), 0 2px 6px rgba(0,0,0,0.6)',
  },
  thumbBig: { boxShadow: '0 0 0 6px rgba(242, 195, 91, 0.3), 0 2px 8px rgba(0,0,0,0.6)' },
  iconButton: { width: 40, height: 40, borderRadius: 12, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(255,255,255,0.06)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.10)' },
  iconButtonHot: { backgroundColor: 'rgba(255,255,255,0.14)' },
  hd: { height: 22, paddingHorizontal: 7, borderRadius: 5, borderWidth: 1.5, borderColor: 'rgba(238, 242, 255, 0.7)', justifyContent: 'center' },
  hdText: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 0.8, color: colors.text },
  speaker: { flexDirection: 'row', alignItems: 'center', width: 22, height: 16 },
  speakerBox: { width: 5, height: 7, borderRadius: 1, backgroundColor: colors.text },
  speakerCone: { width: 6, height: 13, borderTopRightRadius: 3, borderBottomRightRadius: 3, backgroundColor: colors.text, marginLeft: -1 },
  speakerSlash: { position: 'absolute', left: 12, top: 1, width: 2, height: 15, borderRadius: 1, backgroundColor: colors.red, transform: [{ rotate: '35deg' }] },
  wave: { width: 2, borderRadius: 1, backgroundColor: colors.text },

  scenes: { position: 'absolute', left: 48, top: 600, width: PLAYER_W, flexDirection: 'row', gap: 10 },
  chip: { height: 84, padding: 12, paddingTop: 10, borderRadius: 12, borderWidth: 1, borderColor: 'rgba(148, 170, 255, 0.14)', backgroundColor: 'rgba(12, 17, 32, 0.78)', gap: 4 },
  chipHot: { backgroundColor: 'rgba(20, 28, 50, 0.9)' },
  chipCurrent: { borderColor: 'rgba(242, 195, 91, 0.6)', backgroundColor: 'rgba(28, 24, 14, 0.85)', boxShadow: '0 10px 30px rgba(0,0,0,0.45), 0 0 24px rgba(242, 195, 91, 0.12)' },
  chipTop: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  chipNum: { fontFamily: 'JetBrains Mono', fontWeight: '700', fontSize: 12, color: colors.textMute },
  chipTime: { fontFamily: 'JetBrains Mono', fontSize: 11, color: colors.textMute },
  chipTitle: { fontFamily: font, fontWeight: '600', fontSize: 13.5, color: colors.textDim },
  chipBar: { marginTop: 'auto', height: 3, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.08)', overflow: 'hidden' },
  chipFill: { height: 3, backgroundColor: colors.gold },

  side: { position: 'absolute', left: 904, top: 116, width: 328, gap: 12 },
  feedLabel: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', height: 16 },
  feedTitle: { fontFamily: font, fontWeight: '700', fontSize: 10.5, letterSpacing: 1.8, textTransform: 'uppercase', color: colors.textDim },
  feedLive: { flexDirection: 'row', alignItems: 'center', gap: 5 },
  feedLiveText: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.red },
  pip: { width: 328, height: 184, borderRadius: 14, overflow: 'hidden', backgroundColor: '#000', borderWidth: 1, borderColor: 'rgba(79, 209, 255, 0.45)', boxShadow: '0 16px 40px rgba(0,0,0,0.55)' },
  pipFrame: { ...StyleSheet.absoluteFillObject, borderRadius: 14, borderWidth: 2, borderColor: 'rgba(79, 209, 255, 0.18)' },
  feed: { height: 104, borderRadius: 12, overflow: 'hidden', backgroundColor: '#000', borderWidth: 1, borderColor: 'rgba(148, 170, 255, 0.22)' },
  crosshairH: { position: 'absolute', left: '50%', top: '50%', width: 22, height: 1, marginLeft: -11, backgroundColor: 'rgba(255, 77, 106, 0.85)' },
  crosshairV: { position: 'absolute', left: '50%', top: '50%', width: 1, height: 22, marginTop: -11, backgroundColor: 'rgba(255, 77, 106, 0.85)' },
  scanlines: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage: 'repeating-linear-gradient(0deg, rgba(0,0,0,0.14) 0px, rgba(0,0,0,0.14) 1px, rgba(0,0,0,0) 1px, rgba(0,0,0,0) 4px)',
  },
  noSignal: { alignItems: 'center', justifyContent: 'center', backgroundColor: '#070a14' },
  noSignalText: { fontFamily: 'JetBrains Mono', fontSize: 11, letterSpacing: 2, textTransform: 'uppercase', color: colors.textMute },
  info: { padding: 14, borderRadius: 14, backgroundColor: 'rgba(12, 17, 32, 0.82)', borderWidth: 1, borderColor: 'rgba(148, 170, 255, 0.16)', gap: 6 },
  infoTitle: { fontFamily: font, fontWeight: '700', fontSize: 14, color: colors.text },
  infoBody: { fontFamily: font, fontSize: 12, lineHeight: 17, color: colors.textDim },
  statRow: { flexDirection: 'row', gap: 8, marginTop: 4 },
  stat: { flex: 1, paddingVertical: 6, borderRadius: 8, backgroundColor: 'rgba(255,255,255,0.04)', alignItems: 'center' },
  statV: { fontFamily: 'JetBrains Mono', fontWeight: '700', fontSize: 15, color: colors.cyan },
  statK: { fontFamily: font, fontSize: 9.5, letterSpacing: 0.8, textTransform: 'uppercase', color: colors.textMute },

  credit: { paddingLeft: 12, borderLeftWidth: 2, borderLeftColor: 'rgba(242, 195, 91, 0.5)', gap: 3 },
  creditTitle: { fontFamily: font, fontWeight: '700', fontSize: 12, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.text },
  creditText: { fontFamily: font, fontSize: 11.5, lineHeight: 16, color: colors.textDim },
});
