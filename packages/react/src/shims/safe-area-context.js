// Drop-in for react-native-safe-area-context. Insets come from the host's
// `jsui.safeAreaInsets` shared state (the iOS host sets it from
// UIView.safeAreaInsets); hosts that draw into a game's UI texture leave it
// unset, so insets are zero there.
import { createElement as h, createContext, useContext } from 'react';
import { Dimensions } from '../apis.js';
import { SafeAreaView, ZERO_INSETS, useSafeAreaInsets as useHostInsets } from '../safeArea.js';

export { SafeAreaView };
// null: no provider above, read the host value.
export const SafeAreaInsetsContext = createContext(null);
export const SafeAreaFrameContext = createContext(null);
export const initialWindowMetrics = { insets: ZERO_INSETS, frame: { x: 0, y: 0, width: Dimensions.get('window').width, height: Dimensions.get('window').height } };

export function SafeAreaProvider(props) {
  const insets = useHostInsets();
  return h(SafeAreaInsetsContext.Provider, { value: insets }, h('View', { style: [{ flex: 1 }, props.style] }, props.children));
}
export const useSafeAreaInsets = () => {
  const host = useHostInsets();
  return useContext(SafeAreaInsetsContext) || host;
};
export const useSafeAreaFrame = () => {
  const d = Dimensions.get('window');
  return { x: 0, y: 0, width: d.width, height: d.height };
};
export function SafeAreaConsumer(props) {
  return props.children(useSafeAreaInsets());
}
export const withSafeAreaInsets = (C) => (props) => h(C, Object.assign({}, props, { insets: useSafeAreaInsets() }));
