// Shared design tokens for the example game UIs ("Aether" theme): a deep
// navy base, warm gold primary accent, cool cyan secondary, rarity colours.
export const font = 'Inter';

export const colors = {
  bg0: '#070a14',
  bg1: '#0d1322',
  bg2: '#151d33',
  panel: 'rgba(14, 20, 36, 0.92)',
  panelSolid: '#121a2e',
  panelRaised: '#1a2440',
  line: 'rgba(148, 170, 255, 0.16)',
  lineStrong: 'rgba(148, 170, 255, 0.32)',
  text: '#eef2ff',
  textDim: '#a3aecb',
  textMute: '#66718f',
  gold: '#f2c35b',
  goldLight: '#ffe29a',
  goldDeep: '#a8741f',
  cyan: '#4fd1ff',
  cyanDeep: '#1477b8',
  red: '#ff4d6a',
  redDeep: '#9e1730',
  green: '#4ade80',
  greenDeep: '#13803f',
  blue: '#4f8cff',
  purple: '#b26bff',
  orange: '#ff9a3c',
};

export const rarity = {
  common: { color: '#a7b0c0', glow: 'rgba(167, 176, 192, 0.25)', label: 'Common' },
  uncommon: { color: '#4ade80', glow: 'rgba(74, 222, 128, 0.35)', label: 'Uncommon' },
  rare: { color: '#4f9dff', glow: 'rgba(79, 157, 255, 0.45)', label: 'Rare' },
  epic: { color: '#b26bff', glow: 'rgba(178, 107, 255, 0.5)', label: 'Epic' },
  legendary: { color: '#ffb23f', glow: 'rgba(255, 178, 63, 0.55)', label: 'Legendary' },
};

// Text presets. Weights stay within the shipped Inter faces (400-700).
export const type = {
  display: { fontFamily: font, fontWeight: '700', fontSize: 64, letterSpacing: 6, color: colors.text },
  h1: { fontFamily: font, fontWeight: '700', fontSize: 28, letterSpacing: 1, color: colors.text },
  h2: { fontFamily: font, fontWeight: '600', fontSize: 20, color: colors.text },
  label: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textDim },
  body: { fontFamily: font, fontWeight: '400', fontSize: 15, lineHeight: 22, color: colors.textDim },
  small: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute },
  num: { fontFamily: font, fontWeight: '700', fontSize: 16, color: colors.text },
};

export const panel = {
  backgroundColor: colors.panel,
  borderWidth: 1,
  borderColor: colors.line,
  borderRadius: 14,
  boxShadow: '0 18px 40px rgba(0, 0, 0, 0.55), inset 0 1px 0 rgba(255, 255, 255, 0.06)',
};
