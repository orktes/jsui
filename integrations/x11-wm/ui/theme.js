// Design tokens of the touch shell. Two palettes (dark, light) with the same
// keys; useTheme() picks one from the shell's settings.
import { createContext, useContext } from 'react';

export const font = 'Inter';

export const dark = {
  name: 'dark',
  wallTop: '#0b1026',
  wallMid: '#1b1f4b',
  wallBottom: '#3a1d4f',
  glowA: 'rgba(94, 122, 255, 0.55)',
  glowB: 'rgba(255, 112, 166, 0.40)',
  glowC: 'rgba(64, 224, 208, 0.30)',
  bar: 'rgba(10, 13, 28, 0.72)',
  panel: '#161a30', // opaque: at 98.5 % a clock behind a panel showed through
  panelRaised: 'rgba(44, 50, 86, 0.95)',
  panelLine: 'rgba(160, 175, 255, 0.16)',
  chrome: '#1d2140',
  chromeActive: '#272c55',
  chromeLine: 'rgba(170, 185, 255, 0.14)',
  text: '#f2f4ff',
  textDim: '#b3b9da',
  textMute: '#7d84a8',
  accent: '#7c8cff',
  accentStrong: '#5a6cff',
  tileOn: '#4c5ae0',
  accentText: '#ffffff',
  danger: '#ff5a6e',
  ok: '#46d39a',
  warn: '#ffb648',
  scrim: 'rgba(5, 7, 18, 0.62)',
  key: 'rgba(58, 64, 104, 0.96)',
  keyAlt: 'rgba(36, 40, 70, 0.96)',
  shadow: 'rgba(0, 0, 0, 0.55)',
};

export const light = {
  name: 'light',
  wallTop: '#cfe0ff',
  wallMid: '#e9e3ff',
  wallBottom: '#ffe1ec',
  glowA: 'rgba(110, 140, 255, 0.45)',
  glowB: 'rgba(255, 140, 180, 0.40)',
  glowC: 'rgba(80, 220, 200, 0.30)',
  bar: 'rgba(255, 255, 255, 0.70)',
  panel: '#fafbff',
  panelRaised: 'rgba(232, 236, 252, 0.98)',
  panelLine: 'rgba(40, 50, 110, 0.12)',
  chrome: '#eef0fb',
  chromeActive: '#ffffff',
  chromeLine: 'rgba(40, 50, 110, 0.12)',
  text: '#161a33',
  textDim: '#4a5078',
  textMute: '#8086a6',
  accent: '#4b5cff',
  accentStrong: '#3646f0',
  tileOn: '#3646f0',
  accentText: '#ffffff',
  danger: '#e5394f',
  ok: '#1fa874',
  warn: '#d98a00',
  scrim: 'rgba(20, 24, 50, 0.35)',
  key: 'rgba(255, 255, 255, 0.98)',
  keyAlt: 'rgba(214, 219, 240, 0.98)',
  shadow: 'rgba(20, 24, 60, 0.30)',
};

export const ThemeContext = createContext(dark);
export const useTheme = () => useContext(ThemeContext);

// Touch metrics (logical px). 48 px is the smallest comfortable finger target;
// shell controls use 56-72.
export const touch = {
  min: 48,
  button: 56,
  tile: 112,
  radius: 18,
};

// Per-app tile colours for the generated icons, by a hash of the app id.
export const tileColors = [
  ['#5a6cff', '#8a5bff'],
  ['#ff6a88', '#ff9a5a'],
  ['#20c9a6', '#1f8fff'],
  ['#ffb648', '#ff7a45'],
  ['#9b5bff', '#ff5ab4'],
  ['#2fb4ff', '#46d39a'],
  ['#ff5a6e', '#b84bff'],
  ['#3ec6ff', '#5a6cff'],
];
