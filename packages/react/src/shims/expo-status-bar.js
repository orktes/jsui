// Drop-in for expo-status-bar: StatusBar's hidden / style reach the host as
// shared state jsui.statusBar (see StatusBar in components/misc.js).
import { createElement as h } from 'react';
import { StatusBar as RNStatusBar } from '../components/misc.js';

// expo's style: 'auto' | 'inverted' | 'light' | 'dark' (light = light text).
const barStyleOf = (style) => (style === 'light' ? 'light-content' : style === 'dark' ? 'dark-content' : undefined);

export function StatusBar(props) {
  return h(RNStatusBar, { hidden: props.hidden, barStyle: barStyleOf(props.style) });
}
export const setStatusBarStyle = (style) => RNStatusBar.setBarStyle(barStyleOf(style) ?? 'default');
export const setStatusBarHidden = (hidden) => RNStatusBar.setHidden(hidden);
export const setStatusBarBackgroundColor = () => {};
export const setStatusBarTranslucent = () => {};
export const setStatusBarNetworkActivityIndicatorVisible = () => {};
