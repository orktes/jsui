// LottieView: Lottie (Bodymovin JSON) animations with lottie-react-native's
// API, played by the runtime itself (Skia's Skottie, render/lottie.h):
// playback runs on the native frame clock, so a playing animation costs no
// JS per frame.
//
//   import LottieView from 'lottie-react-native';   // the CLI aliases it here
//   <LottieView source={require('./confetti.json')} autoPlay loop style={{ width: 200, height: 200 }} />
//   <LottieView source={{ uri: 'https://example.com/anim.json' }} progress={animatedValue} />
//   ref.current.play(30, 60) / pause() / resume() / reset()
//
// `progress` (0..1) shows that frame instead of playing; an Animated value
// is fine, also with useNativeDriver (it becomes the lottieProgress style
// key, which the native driver can write). Without a size the view takes the
// composition's size (keeping its aspect ratio when one side is set).
// Hosts without native Lottie (react-native-web, Node tests) get an empty
// View of the same style.

import { useImperativeHandle, useRef } from 'react';
import { jsx } from 'react/jsx-runtime';
import { native, warnOnce } from './native.js';
import { resolveAssetSource } from './components/basic.js';
import { AnimatedNode } from './animated/nodes.js';
import { createAnimatedComponent } from './animated/createAnimatedComponent.js';

const hasNative = typeof native.lottieData === 'function' && native.lottie !== false;
const objectKeys = new WeakMap(); // composition object -> native source key
const stringKeys = new Map();     // composition JSON text -> native source key

function dataKey(json, obj) {
  if (obj) {
    let k = objectKeys.get(obj);
    if (!k) objectKeys.set(obj, (k = native.lottieData(JSON.stringify(obj))));
    return k;
  }
  let k = stringKeys.get(json);
  if (!k) stringKeys.set(json, (k = native.lottieData(json)));
  return k;
}

/** The native `source` prop: a URI string, an inline-data key, or null. */
function nativeSource(source) {
  if (source == null) return null;
  if (typeof source === 'number') {
    const a = resolveAssetSource(source);
    return a && a.uri ? String(a.uri) : null;
  }
  if (typeof source === 'string') {
    // JSON text, or the URI of a .json file.
    return /^\s*\{/.test(source) ? dataKey(source, null) : source;
  }
  if (typeof source === 'object') {
    if (source.uri != null) return String(source.uri);
    if (source.default && typeof source.default === 'object' && source.default.layers) return dataKey(null, source.default);
    return dataKey(null, source);
  }
  return null;
}

const AnimatedLottieHost = createAnimatedComponent('LottieView');

const OMIT = {
  source: 1, progress: 1, speed: 1, loop: 1, autoPlay: 1, resizeMode: 1, colorFilters: 1, style: 1, ref: 1,
  onAnimationLoaded: 1, onAnimationFinish: 1, onAnimationFailure: 1,
};

export function LottieView(props) {
  const hostRef = useRef(null);
  useImperativeHandle(props.ref, () => {
    const inst = hostRef.current;
    if (!inst) return null;
    // The ref is the host instance itself (Animated drives it: id,
    // patchStyle), with lottie-react-native's methods on it.
    const cmd = (name, a, b) => {
      if (typeof native.lottieCommand === 'function') native.lottieCommand(inst.id, name, a, b);
    };
    inst.play = (startFrame, endFrame) => cmd('play', startFrame, endFrame);
    inst.pause = () => cmd('pause');
    inst.resume = () => cmd('resume');
    inst.reset = () => cmd('reset');
    return inst;
  });

  const { progress, style } = props;
  const out = {};
  for (const k in props) if (OMIT[k] !== 1) out[k] = props[k];
  out.ref = hostRef;
  if (!hasNative) {
    warnOnce('lottie-unavailable', 'LottieView: this host has no native Lottie support; showing an empty view');
    out.style = style;
    return jsx('View', out);
  }
  out.source = nativeSource(props.source);
  if (props.speed != null) out.speed = +props.speed;
  out.loop = props.loop !== false;
  if (props.autoPlay) out.autoPlay = true;
  if (props.resizeMode) out.resizeMode = props.resizeMode;
  if (props.colorFilters && props.colorFilters.length) out.colorFilters = props.colorFilters;
  if (progress != null) out.style = [style, { lottieProgress: progress }];
  else out.style = style;
  const { onAnimationLoaded, onAnimationFinish, onAnimationFailure } = props;
  if (onAnimationLoaded) out.onLoad = () => onAnimationLoaded();
  if (onAnimationFailure) out.onError = (e) => onAnimationFailure(String(e.nativeEvent.error || 'failed to load'));
  if (onAnimationFinish) out.onAnimationFinish = (e) => onAnimationFinish(!!e.nativeEvent.isCancelled);
  return jsx(progress instanceof AnimatedNode ? AnimatedLottieHost : 'LottieView', out);
}
LottieView.displayName = 'LottieView';
// Animated.createAnimatedComponent(LottieView): an animated `progress` is
// driven as the lottieProgress style key (also by the native driver).
LottieView.__animatedStyleProps = { progress: 'lottieProgress' };

export default LottieView;
