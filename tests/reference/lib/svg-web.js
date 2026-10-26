// Chrome side of react-native-svg cases: the components as plain DOM SVG
// elements (React DOM maps camelCase presentation props such as strokeWidth
// to their attributes), so Chrome's own SVG renderer is the oracle for
// JSUI's VectorView (src/render/vector_view.cpp).
import { createElement } from 'react';

function el(tag) {
  const C = (props) => createElement(tag, props);
  C.displayName = tag;
  return C;
}

export const Svg = (props) => {
  const { style, ...rest } = props;
  return createElement('svg', Object.assign({ xmlns: 'http://www.w3.org/2000/svg', style: { display: 'block', overflow: 'hidden' } }, rest));
};
export const G = el('g');
export const Path = el('path');
export const Rect = el('rect');
export const Circle = el('circle');
export const Ellipse = el('ellipse');
export const Line = el('line');
export const Polyline = el('polyline');
export const Polygon = el('polygon');
export const Text = el('text');
export const TSpan = el('tspan');
export const Defs = el('defs');
export const LinearGradient = el('linearGradient');
export const RadialGradient = el('radialGradient');
export const Stop = el('stop');
export const ClipPath = el('clipPath');
export const Mask = el('mask');
export const Use = el('use');
export const Symbol = el('symbol');
export const Image = el('image');
export default Svg;
