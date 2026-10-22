// Drop-in for react-native-safe-area-context (games have no notches to avoid
// inside the UI texture: insets are zero).
import { createElement as h, createContext, useContext } from 'react';
import { Dimensions } from '../apis.js';

const ZERO = Object.freeze({ top: 0, right: 0, bottom: 0, left: 0 });
export const SafeAreaInsetsContext = createContext(ZERO);
export const SafeAreaFrameContext = createContext(null);
export const initialWindowMetrics = { insets: ZERO, frame: { x: 0, y: 0, width: Dimensions.get('window').width, height: Dimensions.get('window').height } };

export function SafeAreaProvider(props) {
  return h('View', { style: [{ flex: 1 }, props.style] }, props.children);
}
export function SafeAreaView(props) {
  const { edges, mode, ...rest } = props;
  return h('View', rest);
}
export const useSafeAreaInsets = () => useContext(SafeAreaInsetsContext);
export const useSafeAreaFrame = () => {
  const d = Dimensions.get('window');
  return { x: 0, y: 0, width: d.width, height: d.height };
};
export const SafeAreaConsumer = SafeAreaInsetsContext.Consumer;
export const withSafeAreaInsets = (C) => (props) => h(C, Object.assign({}, props, { insets: ZERO }));
