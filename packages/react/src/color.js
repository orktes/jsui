// CSS color parsing for JS-side needs (Animated color interpolation,
// processColor). Native has its own parser for styles.

const NAMED =
  'aliceblue:f0f8ff,antiquewhite:faebd7,aqua:00ffff,aquamarine:7fffd4,azure:f0ffff,beige:f5f5dc,bisque:ffe4c4,' +
  'black:000000,blanchedalmond:ffebcd,blue:0000ff,blueviolet:8a2be2,brown:a52a2a,burlywood:deb887,cadetblue:5f9ea0,' +
  'chartreuse:7fff00,chocolate:d2691e,coral:ff7f50,cornflowerblue:6495ed,cornsilk:fff8dc,crimson:dc143c,cyan:00ffff,' +
  'darkblue:00008b,darkcyan:008b8b,darkgoldenrod:b8860b,darkgray:a9a9a9,darkgreen:006400,darkgrey:a9a9a9,' +
  'darkkhaki:bdb76b,darkmagenta:8b008b,darkolivegreen:556b2f,darkorange:ff8c00,darkorchid:9932cc,darkred:8b0000,' +
  'darksalmon:e9967a,darkseagreen:8fbc8f,darkslateblue:483d8b,darkslategray:2f4f4f,darkslategrey:2f4f4f,' +
  'darkturquoise:00ced1,darkviolet:9400d3,deeppink:ff1493,deepskyblue:00bfff,dimgray:696969,dimgrey:696969,' +
  'dodgerblue:1e90ff,firebrick:b22222,floralwhite:fffaf0,forestgreen:228b22,fuchsia:ff00ff,gainsboro:dcdcdc,' +
  'ghostwhite:f8f8ff,gold:ffd700,goldenrod:daa520,gray:808080,green:008000,greenyellow:adff2f,grey:808080,' +
  'honeydew:f0fff0,hotpink:ff69b4,indianred:cd5c5c,indigo:4b0082,ivory:fffff0,khaki:f0e68c,lavender:e6e6fa,' +
  'lavenderblush:fff0f5,lawngreen:7cfc00,lemonchiffon:fffacd,lightblue:add8e6,lightcoral:f08080,lightcyan:e0ffff,' +
  'lightgoldenrodyellow:fafad2,lightgray:d3d3d3,lightgreen:90ee90,lightgrey:d3d3d3,lightpink:ffb6c1,' +
  'lightsalmon:ffa07a,lightseagreen:20b2aa,lightskyblue:87cefa,lightslategray:778899,lightslategrey:778899,' +
  'lightsteelblue:b0c4de,lightyellow:ffffe0,lime:00ff00,limegreen:32cd32,linen:faf0e6,magenta:ff00ff,maroon:800000,' +
  'mediumaquamarine:66cdaa,mediumblue:0000cd,mediumorchid:ba55d3,mediumpurple:9370db,mediumseagreen:3cb371,' +
  'mediumslateblue:7b68ee,mediumspringgreen:00fa9a,mediumturquoise:48d1cc,mediumvioletred:c71585,' +
  'midnightblue:191970,mintcream:f5fffa,mistyrose:ffe4e1,moccasin:ffe4b5,navajowhite:ffdead,navy:000080,' +
  'oldlace:fdf5e6,olive:808000,olivedrab:6b8e23,orange:ffa500,orangered:ff4500,orchid:da70d6,palegoldenrod:eee8aa,' +
  'palegreen:98fb98,paleturquoise:afeeee,palevioletred:db7093,papayawhip:ffefd5,peachpuff:ffdab9,peru:cd853f,' +
  'pink:ffc0cb,plum:dda0dd,powderblue:b0e0e6,purple:800080,rebeccapurple:663399,red:ff0000,rosybrown:bc8f8f,' +
  'royalblue:4169e1,saddlebrown:8b4513,salmon:fa8072,sandybrown:f4a460,seagreen:2e8b57,seashell:fff5ee,' +
  'sienna:a0522d,silver:c0c0c0,skyblue:87ceeb,slateblue:6a5acd,slategray:708090,slategrey:708090,snow:fffafa,' +
  'springgreen:00ff7f,steelblue:4682b4,tan:d2b48c,teal:008080,thistle:d8bfd8,tomato:ff6347,turquoise:40e0d0,' +
  'violet:ee82ee,wheat:f5deb3,white:ffffff,whitesmoke:f5f5f5,yellow:ffff00,yellowgreen:9acd32';

let named = null;
function namedColor(name) {
  if (!named) {
    named = new Map();
    for (const pair of NAMED.split(',')) {
      const [k, v] = pair.split(':');
      named.set(k, v);
    }
  }
  return named.get(name);
}

function hue(p, q, t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1 / 6) return p + (q - p) * 6 * t;
  if (t < 1 / 2) return q;
  if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
  return p;
}

const cache = new Map();

/**
 * Parses a CSS color into [r, g, b, a] (0..255, alpha 0..1), or null.
 * Results are cached: interpolation re-parses its ranges only once anyway,
 * but processColor may be called per render.
 */
export function parseColor(input) {
  if (typeof input === 'number') {
    return [(input >>> 16) & 255, (input >>> 8) & 255, input & 255, ((input >>> 24) & 255) / 255];
  }
  if (typeof input !== 'string') return null;
  let hit = cache.get(input);
  if (hit !== undefined) return hit;
  hit = parseUncached(input.trim().toLowerCase());
  if (cache.size > 512) cache.clear();
  cache.set(input, hit);
  return hit;
}

function parseUncached(s) {
  if (s === 'transparent') return [0, 0, 0, 0];
  if (s[0] !== '#') {
    const n = namedColor(s);
    if (n) s = '#' + n;
  }
  if (s[0] === '#') {
    const h = s.slice(1);
    if (!/^[0-9a-f]+$/.test(h)) return null;
    if (h.length === 3 || h.length === 4) {
      const v = h.split('').map((c) => parseInt(c + c, 16));
      return [v[0], v[1], v[2], h.length === 4 ? v[3] / 255 : 1];
    }
    if (h.length === 6 || h.length === 8) {
      const v = [0, 2, 4, 6].map((i) => parseInt(h.slice(i, i + 2), 16));
      return [v[0], v[1], v[2], h.length === 8 ? v[3] / 255 : 1];
    }
    return null;
  }
  const m = /^(rgba?|hsla?)\(([^)]*)\)$/.exec(s);
  if (!m) return null;
  const parts = m[2].split(/[\s,/]+/).filter(Boolean);
  if (parts.length < 3) return null;
  const num = (p, scale) => (p.endsWith('%') ? (parseFloat(p) / 100) * scale : parseFloat(p));
  const a = parts.length > 3 ? num(parts[3], 1) : 1;
  if (m[1][0] === 'r') {
    return [num(parts[0], 255), num(parts[1], 255), num(parts[2], 255), a].map((v, i) =>
      i < 3 ? Math.max(0, Math.min(255, v)) : Math.max(0, Math.min(1, v)),
    );
  }
  const H = (((parseFloat(parts[0]) % 360) + 360) % 360) / 360;
  const S = parseFloat(parts[1]) / 100;
  const L = parseFloat(parts[2]) / 100;
  const q = L < 0.5 ? L * (1 + S) : L + S - L * S;
  const p = 2 * L - q;
  return [hue(p, q, H + 1 / 3) * 255, hue(p, q, H) * 255, hue(p, q, H - 1 / 3) * 255, Math.max(0, Math.min(1, a))];
}

/** RN processColor: 0xAARRGGBB as an unsigned number, or null. */
export function processColor(color) {
  if (color == null) return color;
  const c = parseColor(color);
  if (!c) return null;
  return (((Math.round(c[3] * 255) << 24) | (Math.round(c[0]) << 16) | (Math.round(c[1]) << 8) | Math.round(c[2])) >>> 0);
}
