// Site design tokens. The game screens keep the "Aether" theme from
// examples/theme.js; the site chrome around them uses the same palette so the
// whole page reads as one product.
export { colors, rarity, type as gameType, panel as gamePanel, font } from '../../examples/theme.js';
import { colors, font } from '../../examples/theme.js';

export const mono = 'JetBrains Mono';

export const site = {
  bg: '#05070f',
  bgRaised: '#0a0f1d',
  card: 'rgba(16, 22, 40, 0.86)',
  cardSolid: '#10172a',
  stroke: 'rgba(148, 170, 255, 0.14)',
  strokeStrong: 'rgba(148, 170, 255, 0.28)',
  accent: colors.gold,
  accent2: colors.cyan,
  code: '#0b1020',
};

export const text = {
  hero: { fontFamily: font, fontWeight: '700', fontSize: 64, lineHeight: 70, letterSpacing: -1.5, color: colors.text },
  h1: { fontFamily: font, fontWeight: '700', fontSize: 40, lineHeight: 46, letterSpacing: -0.8, color: colors.text },
  h2: { fontFamily: font, fontWeight: '700', fontSize: 26, lineHeight: 32, letterSpacing: -0.3, color: colors.text },
  h3: { fontFamily: font, fontWeight: '600', fontSize: 18, lineHeight: 24, color: colors.text },
  lead: { fontFamily: font, fontWeight: '400', fontSize: 19, lineHeight: 29, color: colors.textDim },
  body: { fontFamily: font, fontWeight: '400', fontSize: 15, lineHeight: 24, color: colors.textDim },
  small: { fontFamily: font, fontWeight: '500', fontSize: 13, lineHeight: 18, color: colors.textMute },
  eyebrow: { fontFamily: font, fontWeight: '700', fontSize: 12, letterSpacing: 2.4, textTransform: 'uppercase', color: colors.gold },
  code: { fontFamily: mono, fontSize: 13, lineHeight: 20, color: '#d6def5' },
};

// Breakpoints in logical px (the canvas' CSS size).
export const bp = { phone: 640, tablet: 980, wide: 1280 };
