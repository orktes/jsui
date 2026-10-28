// The web entry of react-native-skia (and @shopify/react-native-skia):
// 'react-native-skia/lib/module/web'. JSUI draws Skia natively on every
// host, CanvasKit included, so there is nothing to load first.
import { Suspense, createElement, lazy, useMemo } from 'react';

export const LoadSkiaWeb = () => Promise.resolve();

export function WithSkiaWeb({ getComponent, fallback = null, componentProps }) {
  const Inner = useMemo(() => lazy(() => Promise.resolve(getComponent())), []);
  return createElement(Suspense, { fallback }, createElement(Inner, componentProps));
}
