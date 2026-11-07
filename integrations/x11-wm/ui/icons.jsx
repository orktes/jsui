// Line icons for the shell, drawn on a 24x24 grid with react-native-svg
// (round caps and joins, stroke = currentColor-like `color` prop).
import React, { memo } from 'react';
import { View } from 'react-native';
import Svg, { Circle, Line, Path, Polyline, Rect } from 'react-native-svg';
import { tileColors } from './theme.js';

const shapes = {
  grid: (
    <>
      <Rect x="3.5" y="3.5" width="7" height="7" rx="2" />
      <Rect x="13.5" y="3.5" width="7" height="7" rx="2" />
      <Rect x="3.5" y="13.5" width="7" height="7" rx="2" />
      <Rect x="13.5" y="13.5" width="7" height="7" rx="2" />
    </>
  ),
  close: (
    <>
      <Line x1="6" y1="6" x2="18" y2="18" />
      <Line x1="18" y1="6" x2="6" y2="18" />
    </>
  ),
  minimize: <Line x1="6" y1="16" x2="18" y2="16" />,
  maximize: <Rect x="5.5" y="5.5" width="13" height="13" rx="2.5" />,
  restore: (
    <>
      <Rect x="4.5" y="8.5" width="11" height="11" rx="2" />
      <Path d="M8.5 5.5h8a2.5 2.5 0 0 1 2.5 2.5v8" />
    </>
  ),
  overview: (
    <>
      <Rect x="7.5" y="4.5" width="9" height="15" rx="2" />
      <Path d="M4 7v10M20 7v10" />
    </>
  ),
  wifi: (
    <>
      <Path d="M2.5 9a14 14 0 0 1 19 0" />
      <Path d="M5.5 12.5a9.5 9.5 0 0 1 13 0" />
      <Path d="M8.6 15.9a5 5 0 0 1 6.8 0" />
      <Circle cx="12" cy="19" r="1.2" />
    </>
  ),
  rotate: (
    <>
      <Rect x="7.5" y="3.5" width="9" height="15" rx="2" />
      <Path d="M20.5 13.5a8.5 8.5 0 0 1-6 7" />
      <Polyline points="14 18 14 20.6 16.6 20.6" />
    </>
  ),
  bluetooth: <Polyline points="7 7 17 16 12 20.5 12 3.5 17 8 7 17" />,
  volume: (
    <>
      <Path d="M4 9.5h3.5L12 5.5v13l-4.5-4H4z" />
      <Path d="M15.5 9a4 4 0 0 1 0 6" />
      <Path d="M18 6.5a7.5 7.5 0 0 1 0 11" />
    </>
  ),
  mute: (
    <>
      <Path d="M4 9.5h3.5L12 5.5v13l-4.5-4H4z" />
      <Line x1="16" y1="9.5" x2="21" y2="14.5" />
      <Line x1="21" y1="9.5" x2="16" y2="14.5" />
    </>
  ),
  sun: (
    <>
      <Circle cx="12" cy="12" r="4" />
      <Path d="M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M5.3 18.7l1.4-1.4M17.3 6.7l1.4-1.4" />
    </>
  ),
  moon: <Path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z" />,
  // Dark style: a circle, its right half hatched dark.
  contrast: (
    <>
      <Circle cx="12" cy="12" r="8.5" />
      <Path d="M12 3.5v17M15.2 4.2v15.6M18.2 6v12" />
    </>
  ),
  // Night light: a sun setting on the horizon.
  sunset: <Path d="M3.5 17.5h17M7 17.5a5 5 0 0 1 10 0M12 6.5v2.5M5.4 10.4l1.6 1.6M18.6 10.4L17 12" />,
  bell: (
    <>
      <Path d="M6 16.5V11a6 6 0 0 1 12 0v5.5l1.5 2h-15z" />
      <Path d="M10 20.5a2 2 0 0 0 4 0" />
    </>
  ),
  keyboard: (
    <>
      <Rect x="2.5" y="6" width="19" height="12" rx="2.5" />
      <Path d="M6 9.5h.01M9.3 9.5h.01M12.6 9.5h.01M15.9 9.5h.01M18 9.5h.01M7.5 14.5h9" />
    </>
  ),
  power: (
    <>
      <Path d="M7.5 6.5a7.5 7.5 0 1 0 9 0" />
      <Line x1="12" y1="3" x2="12" y2="11" />
    </>
  ),
  lock: (
    <>
      <Rect x="5" y="10.5" width="14" height="10" rx="2.5" />
      <Path d="M8 10.5V8a4 4 0 0 1 8 0v2.5" />
    </>
  ),
  search: (
    <>
      <Circle cx="10.5" cy="10.5" r="6" />
      <Line x1="15" y1="15" x2="20.5" y2="20.5" />
    </>
  ),
  tablet: (
    <>
      <Rect x="3" y="5" width="18" height="14" rx="2.5" />
      <Line x1="10.5" y1="16" x2="13.5" y2="16" />
    </>
  ),
  refresh: (
    <>
      <Path d="M19.5 12a7.5 7.5 0 1 1-2.2-5.3" />
      <Polyline points="20 4 20 8.5 15.5 8.5" />
    </>
  ),
  logout: (
    <>
      <Path d="M14 4.5H6.5a2 2 0 0 0-2 2v11a2 2 0 0 0 2 2H14" />
      <Polyline points="16 8 20 12 16 16" />
      <Line x1="9.5" y1="12" x2="20" y2="12" />
    </>
  ),
  plus: (
    <>
      <Line x1="12" y1="5" x2="12" y2="19" />
      <Line x1="5" y1="12" x2="19" y2="12" />
    </>
  ),
  backspace: (
    <>
      <Path d="M8.5 5.5h11a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2h-11L2.5 12z" />
      <Line x1="11.5" y1="9.5" x2="16.5" y2="14.5" />
      <Line x1="16.5" y1="9.5" x2="11.5" y2="14.5" />
    </>
  ),
  enter: (
    <>
      <Path d="M19.5 5v6a3 3 0 0 1-3 3h-12" />
      <Polyline points="8.5 10 4.5 14 8.5 18" />
    </>
  ),
  shift: <Path d="M12 4l8 8.5h-4.5V20h-7v-7.5H4z" />,
  chevronDown: <Polyline points="6 9.5 12 15.5 18 9.5" />,
  chevronUp: <Polyline points="6 14.5 12 8.5 18 14.5" />,
  left: <Polyline points="14.5 6 8.5 12 14.5 18" />,
  right: <Polyline points="9.5 6 15.5 12 9.5 18" />,
  snapLeft: (
    <>
      <Rect x="3.5" y="5.5" width="17" height="13" rx="2" />
      <Path d="M12 5.5v13" />
      <Rect x="5.5" y="7.5" width="4.5" height="9" rx="1" />
    </>
  ),
  snapRight: (
    <>
      <Rect x="3.5" y="5.5" width="17" height="13" rx="2" />
      <Path d="M12 5.5v13" />
      <Rect x="14" y="7.5" width="4.5" height="9" rx="1" />
    </>
  ),
  // The same on a portrait screen, where split view stacks.
  snapTop: (
    <>
      <Rect x="5.5" y="3.5" width="13" height="17" rx="2" />
      <Path d="M5.5 12h13" />
      <Rect x="7.5" y="5.5" width="9" height="4.5" rx="1" />
    </>
  ),
  snapBottom: (
    <>
      <Rect x="5.5" y="3.5" width="13" height="17" rx="2" />
      <Path d="M5.5 12h13" />
      <Rect x="7.5" y="14" width="9" height="4.5" rx="1" />
    </>
  ),
  // App glyphs for the generated icons.
  terminal: (
    <>
      <Rect x="3" y="4.5" width="18" height="15" rx="3" />
      <Polyline points="7 9.5 10 12 7 14.5" />
      <Line x1="12" y1="15" x2="16.5" y2="15" />
    </>
  ),
  calculator: (
    <>
      <Rect x="5" y="3" width="14" height="18" rx="3" />
      <Rect x="8" y="6" width="8" height="3.5" rx="1" />
      <Path d="M8.5 13h.01M12 13h.01M15.5 13h.01M8.5 17h.01M12 17h.01M15.5 17h.01" />
    </>
  ),
  clock: (
    <>
      <Circle cx="12" cy="12" r="8.5" />
      <Polyline points="12 7 12 12 15.5 14" />
    </>
  ),
  eye: (
    <>
      <Path d="M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12z" />
      <Circle cx="12" cy="12" r="3" />
    </>
  ),
  edit: (
    <>
      <Path d="M14 3.5H7a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8.5z" />
      <Polyline points="14 3.5 14 8.5 19 8.5" />
      <Path d="M8.5 13h7M8.5 16.5h5" />
    </>
  ),
  folder: <Path d="M3.5 7a2 2 0 0 1 2-2h4l2 2.5h7a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z" />,
  logo: (
    <>
      <Path d="M5 5l14 14M19 5L5 19" />
      <Circle cx="12" cy="12" r="8.5" />
    </>
  ),
  app: (
    <>
      <Rect x="4" y="4" width="16" height="16" rx="4" />
      <Circle cx="12" cy="12" r="3.5" />
    </>
  ),
  settings: (
    <>
      <Circle cx="12" cy="12" r="3.2" />
      <Path d="M12 2.8v2.6M12 18.6v2.6M2.8 12h2.6M18.6 12h2.6M5.5 5.5l1.9 1.9M16.6 16.6l1.9 1.9M5.5 18.5l1.9-1.9M16.6 7.4l1.9-1.9" />
    </>
  ),
  globe: (
    <>
      <Circle cx="12" cy="12" r="8.5" />
      <Path d="M3.5 12h17M12 3.5c2.5 2.6 3.5 5.4 3.5 8.5s-1 5.9-3.5 8.5c-2.5-2.6-3.5-5.4-3.5-8.5s1-5.9 3.5-8.5z" />
    </>
  ),
  image: (
    <>
      <Rect x="3.5" y="4.5" width="17" height="15" rx="2.5" />
      <Circle cx="9" cy="9.5" r="1.8" />
      <Polyline points="20.5 15.5 15.5 11 5.5 19.5" />
    </>
  ),
  battery: (
    <>
      <Rect x="2.5" y="7" width="17" height="10" rx="2.5" />
      <Line x1="22" y1="10.5" x2="22" y2="13.5" />
    </>
  ),
  user: (
    <>
      <Circle cx="12" cy="8.5" r="4" />
      <Path d="M4.5 20.5a7.5 7.5 0 0 1 15 0" />
    </>
  ),
  code: (
    <>
      <Polyline points="8.5 7 3.5 12 8.5 17" />
      <Polyline points="15.5 7 20.5 12 15.5 17" />
      <Line x1="13.5" y1="5" x2="10.5" y2="19" />
    </>
  ),
  doc: (
    <>
      <Path d="M6.5 3.5h7l4 4v13h-11z" />
      <Line x1="9" y1="12" x2="15" y2="12" />
      <Line x1="9" y1="16" x2="15" y2="16" />
    </>
  ),
  sheet: (
    <>
      <Rect x="3.5" y="4.5" width="17" height="15" rx="2.5" />
      <Line x1="3.5" y1="9.5" x2="20.5" y2="9.5" />
      <Line x1="3.5" y1="14.5" x2="20.5" y2="14.5" />
      <Line x1="9.5" y1="4.5" x2="9.5" y2="19.5" />
    </>
  ),
  present: (
    <>
      <Rect x="3.5" y="4.5" width="17" height="11" rx="2" />
      <Line x1="12" y1="15.5" x2="12" y2="20" />
      <Line x1="8.5" y1="20" x2="15.5" y2="20" />
    </>
  ),
  pen: (
    <>
      <Path d="M15.5 4.5l4 4L9 19H5v-4z" />
      <Line x1="13" y1="7" x2="17" y2="11" />
    </>
  ),
  more: (
    <>
      <Circle cx="6" cy="12" r="1.2" />
      <Circle cx="12" cy="12" r="1.2" />
      <Circle cx="18" cy="12" r="1.2" />
    </>
  ),
  plug: (
    <>
      <Path d="M8 3.5v4M16 3.5v4" />
      <Path d="M6 7.5h12v3a6 6 0 0 1-12 0z" />
      <Line x1="12" y1="16.5" x2="12" y2="20.5" />
    </>
  ),
};

export const Icon = memo(function Icon({ name, size = 24, color = '#fff', strokeWidth = 2, fill = 'none' }) {
  return (
    <Svg width={size} height={size} viewBox="0 0 24 24" fill={fill} stroke={color} strokeWidth={strokeWidth}
      strokeLinecap="round" strokeLinejoin="round">
      {shapes[name] || shapes.app}
    </Svg>
  );
});

// Which glyph an app gets when it has no icon file: by its id / exec / class.
export function glyphFor(text) {
  const s = String(text || '').toLowerCase();
  if (/libreoffice-calc|spreadsheet/.test(s)) return 'sheet';
  if (/libreoffice-impress|presentation/.test(s)) return 'present';
  if (/libreoffice-draw/.test(s)) return 'pen';
  if (/libreoffice|writer|document/.test(s)) return 'doc';
  if (/\bvim\b|emacs|nano|python|code/.test(s)) return 'code';
  if (/term|xterm|console|shell|bash|urxvt|kitty|alacritty/.test(s)) return 'terminal';
  if (/calc/.test(s)) return 'calculator';
  if (/clock|time/.test(s)) return 'clock';
  if (/eye/.test(s)) return 'eye';
  if (/setting|config|control|pref/.test(s)) return 'settings';
  if (/edit|pad|text|write|note/.test(s)) return 'edit';
  if (/file|folder|pcmanfm|thunar|nautilus|explorer/.test(s)) return 'folder';
  if (/logo/.test(s)) return 'logo';
  if (/web|browser|firefox|chrom|surf/.test(s)) return 'globe';
  if (/image|photo|paint|draw|view|display-im/.test(s)) return 'image';
  return 'app';
}

// One colour pair per kind of app, so the same kind always looks the same.
const glyphColors = {
  terminal: ['#2b3150', '#14182c'],
  code: ['#2fb46e', '#13795b'],
  calculator: ['#ff9a3c', '#ff5f45'],
  clock: ['#5a6cff', '#8a5bff'],
  eye: ['#20c9a6', '#1f8fff'],
  edit: ['#ffb648', '#ff7a45'],
  folder: ['#3ec6ff', '#2f7bff'],
  logo: ['#9b5bff', '#ff5ab4'],
  globe: ['#2fb4ff', '#46d39a'],
  image: ['#ff5ab4', '#b84bff'],
  settings: ['#7d84a8', '#4a5078'],
  doc: ['#3b82f6', '#1d4ed8'],
  sheet: ['#22c55e', '#15803d'],
  present: ['#f97316', '#c2410c'],
  pen: ['#eab308', '#ca8a04'],
};

function hash(s) {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}

// A generated app icon: a gradient squircle with a white glyph.
export const AppGlyph = memo(function AppGlyph({ id, glyph, size = 56 }) {
  const [a, b] = glyphColors[glyph] || tileColors[hash(String(id || glyph || '')) % tileColors.length];
  return (
    <View
      style={{
        width: size,
        height: size,
        borderRadius: size * 0.28,
        alignItems: 'center',
        justifyContent: 'center',
        experimental_backgroundImage: `linear-gradient(135deg, ${a}, ${b})`,
        boxShadow: `0px ${size * 0.06}px ${size * 0.18}px rgba(0,0,0,0.28)`,
      }}
    >
      <Icon name={glyph || glyphFor(id)} size={size * 0.58} color="#fff" strokeWidth={2.1} />
    </View>
  );
});
