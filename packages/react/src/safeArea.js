// Safe-area insets (notches, Dynamic Island, home indicator, rounded
// corners). The host publishes them as shared state `jsui.safeAreaInsets`
// ({top, right, bottom, left} in logical px; jsui_set_state), e.g. the iOS
// host from UIView.safeAreaInsets. Hosts without one (a game's UI texture)
// leave it unset: insets are zero.
import { createElement as h } from 'react';
import { useHostState } from './hostData.js';
import { flatten } from './StyleSheet.js';

export const SAFE_AREA_KEY = 'jsui.safeAreaInsets';
export const ZERO_INSETS = Object.freeze({ top: 0, right: 0, bottom: 0, left: 0 });

function normalize(v) {
  if (!v || typeof v !== 'object') return ZERO_INSETS;
  return {
    top: +v.top || 0,
    right: +v.right || 0,
    bottom: +v.bottom || 0,
    left: +v.left || 0,
  };
}

let last = null;
let lastInsets = ZERO_INSETS;
// Same object while the host value is unchanged (stable for effect deps).
function insetsOf(v) {
  if (v !== last) {
    last = v;
    lastInsets = normalize(v);
  }
  return lastInsets;
}

export function useSafeAreaInsets() {
  return insetsOf(useHostState(SAFE_AREA_KEY, null));
}

const ALL_EDGES = ['top', 'right', 'bottom', 'left'];
const PAD = { top: 'paddingTop', right: 'paddingRight', bottom: 'paddingBottom', left: 'paddingLeft' };
const MARGIN = { top: 'marginTop', right: 'marginRight', bottom: 'marginBottom', left: 'marginLeft' };

// React Native's SafeAreaView (all edges) and react-native-safe-area-context's
// (`edges`: array or {top: 'additive' | 'maximum' | 'off', ...}; `mode`:
// 'padding' | 'margin'). Insets are added to the style's own padding / margin.
export function SafeAreaView(props) {
  const { edges, mode, style, ...rest } = props;
  const insets = useSafeAreaInsets();
  const extra = {};
  const names = mode === 'margin' ? MARGIN : PAD;
  const list = Array.isArray(edges)
    ? edges
    : edges && typeof edges === 'object'
      ? ALL_EDGES.filter((e) => edges[e] && edges[e] !== 'off')
      : ALL_EDGES;
  for (const e of list) if (insets[e]) extra[names[e]] = insets[e];
  if (Object.keys(extra).length === 0) return h('View', { ...rest, style });
  return h('View', { ...rest, style: [style, addTo(style, extra)] });
}

// Inset + the style's own value for the same edge (flattened, longhand or
// shorthand), so `padding: 16` stays 16 px inside the safe area.
function addTo(style, extra) {
  const flat = flatten(style) || {};
  const out = {};
  for (const k in extra) {
    const isPad = k.startsWith('padding');
    const base = isPad ? 'padding' : 'margin';
    const axis = k.endsWith('Top') || k.endsWith('Bottom') ? base + 'Vertical' : base + 'Horizontal';
    const own = flat[k] ?? flat[axis] ?? flat[base];
    out[k] = extra[k] + (typeof own === 'number' ? own : 0);
  }
  return out;
}

SafeAreaView.displayName = 'SafeAreaView';
