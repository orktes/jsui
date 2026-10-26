// Video: react-native-video's <Video>, played by the host (include/jsui/jsui.h
// "Video"). JSUI decodes no video: the runtime hands the view's requests to
// the host's video handler (the browser: a <video> element; jsui-viewer on
// macOS: AVFoundation; a game engine: its own player), which draws the
// frames into a host texture the view shows like an Image. No JS runs per
// frame; onProgress comes at progressUpdateInterval.
//
//   import Video from 'react-native-video';   // the CLI aliases it here
//   <Video source={{ uri: 'https://example.com/trailer.m3u8' }} style={{ width: 640, aspectRatio: 16 / 9 }}
//          paused={!playing} muted repeat resizeMode="cover"
//          onLoad={({ duration, naturalSize }) => ...} onProgress={({ currentTime }) => ...} onEnd={...} />
//   ref.current.seek(30) / pause() / resume()
//
// `source` is a URL ({ uri }, a string or an imported video file) or a list
// of { uri, type } alternatives (type: a MIME type such as 'video/webm;
// codecs="vp9"' or 'application/vnd.apple.mpegurl'): the host plays the
// first it can. `poster` (an image source) shows until the first frame. The frames
// also feed host texture `textureName` (default: one per view) for other
// <HostTexture> views, and buffer `${textureName}.time` holds [position /
// duration, buffered / duration, position s] for Animated.hostBuffer
// progress bars. Without a video handler (or the native bridge: Node tests,
// react-native-web) the view stays empty and onError fires.

import { useEffect, useImperativeHandle, useRef, useState } from 'react';
import { jsx } from 'react/jsx-runtime';
import { native } from './native.js';
import { Image, resolveAssetSource } from './components/basic.js';
import { HostTexture } from './hostTexture.js';

const g = globalThis;
const hasVideo = typeof native.videoOpen === 'function';

// JsuiVideoState
export const VideoState = Object.freeze({ loading: 0, paused: 1, playing: 2, buffering: 3, ended: 4, failed: -1 });

const views = new Map(); // video id -> status callback
let nextTexture = 1;

// Native reports: [[video, state, duration, position, buffered, width, height, error], ...]
g.__jsui_onVideo = function (events) {
  for (const e of events) {
    const cb = views.get(e[0]);
    if (cb) cb({ state: e[1], duration: e[2], position: e[3], buffered: e[4], width: e[5], height: e[6], error: e[7] });
  }
};

function uriOf(s) {
  if (s == null) return null;
  if (typeof s === 'number' || typeof s === 'string') {
    const a = resolveAssetSource(s);
    return a && a.uri ? String(a.uri) : null;
  }
  return s.uri != null ? String(s.uri) : null;
}

// [uris, types]: the alternatives in preference order (types: MIME type or null).
function sourcesOf(source) {
  const list = (Array.isArray(source) ? source : [source])
    .map((s) => ({ uri: uriOf(s), type: (s && typeof s === 'object' && s.type) || null }))
    .filter((s) => s.uri);
  return [list.map((s) => s.uri), list.map((s) => s.type)];
}

export function Video(props) {
  const {
    source,
    paused = false,
    muted = false,
    volume = 1,
    repeat = false,
    rate = 1,
    resizeMode = 'contain',
    progressUpdateInterval = 250,
    textureName,
    poster,
    posterResizeMode,
    onLoadStart,
    onLoad,
    onProgress,
    onEnd,
    onError,
    onBuffer,
    onPlaybackStateChanged,
    onReadyForDisplay,
    style,
    ref,
    ...rest
  } = props;
  const [own] = useState(() => `jsui.video.${nextTexture++}`);
  const texture = textureName || own;
  const [hasFrame, setHasFrame] = useState(false);
  const id = useRef(0);
  const latest = useRef(props);
  latest.current = props;
  const [uris, types] = sourcesOf(source);
  const key = JSON.stringify([uris, types]);

  // Open per source; the playback props follow with videoSet.
  useEffect(() => {
    if (uris.length === 0) return undefined;
    setHasFrame(false);
    const p = latest.current;
    if (!hasVideo) {
      p.onError?.({ error: { errorString: 'video is not available here (no native bridge)' } });
      return undefined;
    }
    let last = { state: VideoState.loading, duration: 0, position: 0, buffered: 0, width: 0, height: 0 };
    let loaded = false;
    const vid = native.videoOpen(uris, types, texture, !!p.paused, !!p.muted, p.volume ?? 1, !!p.repeat, p.rate ?? 1,
      p.progressUpdateInterval ?? 250);
    id.current = vid;
    p.onLoadStart?.({ isNetwork: /^https?:/.test(uris[0]), uri: uris[0] });
    views.set(vid, (s) => {
      const q = latest.current;
      const prev = last;
      last = s;
      if (s.state === VideoState.failed) {
        if (prev.state !== VideoState.failed) q.onError?.({ error: { errorString: s.error || 'video failed' } });
        return;
      }
      // Loaded: ready to play (paused, playing or at the end).
      if (!loaded && (s.state === VideoState.paused || s.state === VideoState.playing || s.state === VideoState.ended)) {
        loaded = true;
        q.onLoad?.({ currentTime: s.position, duration: s.duration,
          naturalSize: { width: s.width, height: s.height, orientation: s.width >= s.height ? 'landscape' : 'portrait' } });
      }
      if (s.width > 0 && !(prev.width > 0)) {
        setHasFrame(true);
        q.onReadyForDisplay?.();
      }
      const buffering = s.state === VideoState.buffering || s.state === VideoState.loading;
      const wasBuffering = prev.state === VideoState.buffering || prev.state === VideoState.loading;
      if (buffering !== wasBuffering) q.onBuffer?.({ isBuffering: buffering });
      const playing = s.state === VideoState.playing;
      if (playing !== (prev.state === VideoState.playing)) q.onPlaybackStateChanged?.({ isPlaying: playing });
      if (s.position !== prev.position || s.buffered !== prev.buffered)
        q.onProgress?.({ currentTime: s.position, playableDuration: s.buffered, seekableDuration: s.duration });
      if (s.state === VideoState.ended && prev.state !== VideoState.ended) q.onEnd?.();
    });
    return () => {
      views.delete(vid);
      native.videoClose(vid);
      if (id.current === vid) id.current = 0;
    };
  }, [key, texture]);

  useEffect(() => {
    if (id.current) native.videoSet(id.current, !!paused, !!muted, volume, !!repeat, rate);
  }, [paused, muted, volume, repeat, rate]);

  useImperativeHandle(ref, () => ({
    seek: (time) => id.current && native.videoSeek(id.current, +time || 0),
    pause: () => id.current && native.videoSet(id.current, true, !!latest.current.muted, latest.current.volume ?? 1,
      !!latest.current.repeat, latest.current.rate ?? 1),
    resume: () => id.current && native.videoSet(id.current, false, !!latest.current.muted,
      latest.current.volume ?? 1, !!latest.current.repeat, latest.current.rate ?? 1),
    textureName: texture,
  }), [texture]);

  // The poster in the picture's place until the first frame.
  if (poster && !hasFrame) {
    return jsx(Image, { ...rest, source: typeof poster === 'string' ? { uri: poster } : poster,
      resizeMode: posterResizeMode || resizeMode, style });
  }
  return jsx(HostTexture, { ...rest, name: texture, resizeMode, style });
}

export default Video;
