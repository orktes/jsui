// Drop-in for expo-linear-gradient / react-native-linear-gradient, rendered
// natively by the painter as a CSS linear-gradient background.
import { createElement as h } from 'react';

function toCss(colors, locations, start, end) {
  const s = Array.isArray(start) ? { x: start[0], y: start[1] } : start || { x: 0.5, y: 0 };
  const e = Array.isArray(end) ? { x: end[0], y: end[1] } : end || { x: 0.5, y: 1 };
  // CSS angle: 0deg points up, clockwise. Vector (dx, dy) in a unit box.
  const angle = (Math.atan2(e.x - s.x, -(e.y - s.y)) * 180) / Math.PI;
  const stops = (colors || []).map((c, i) => (locations && locations[i] != null ? `${c} ${locations[i] * 100}%` : c));
  return `linear-gradient(${Math.round(angle * 100) / 100}deg, ${stops.join(', ')})`;
}

export function LinearGradient(props) {
  const { colors, locations, start, end, style, dither, ...rest } = props;
  return h('View', Object.assign(rest, { style: [style, { experimental_backgroundImage: toCss(colors, locations, start, end) }] }));
}
LinearGradient.displayName = 'LinearGradient';
export default LinearGradient;
