// Drop-in for expo-font, backed by JSUI's Fonts. With the jsui CLI,
// require('./Font.ttf') is a URL, so @expo-google-fonts/* packages work as is.
import { Fonts, useFonts } from '../apis.js';

export { useFonts };
export const loadAsync = (nameOrMap, source) => Fonts.loadAsync(nameOrMap, source);
export const isLoaded = (family) => Fonts.isLoaded(family);
export const isLoading = (family) => Fonts.isLoading(family);
export const getLoadedFonts = () => Fonts.getLoadedFonts();
export const unloadAsync = (nameOrMap) => Fonts.unloadAsync(nameOrMap);
export const unloadAllAsync = () => Fonts.unloadAllAsync();

/** expo-font's FontDisplay (web font-display); accepted and ignored: JSUI re-lays out text when a font arrives. */
export const FontDisplay = Object.freeze({ AUTO: 'auto', SWAP: 'swap', BLOCK: 'block', FALLBACK: 'fallback', OPTIONAL: 'optional' });
