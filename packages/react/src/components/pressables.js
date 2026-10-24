// Pressable, Touchable*, Button.

import { createElement as h, cloneElement, isValidElement, Children, useRef, useEffect } from 'react';
import { jsx } from 'react/jsx-runtime';
import { usePress, PRESS_PROPS, omit } from './usePress.js';
import { flatten, StyleSheet } from '../StyleSheet.js';
import { Text } from './basic.js';
import { AnimatedValue } from '../animated/nodes.js';
import { timing } from '../animated/animations.js';
import { Easing } from '../animated/Easing.js';
import { createAnimatedComponent } from '../animated/createAnimatedComponent.js';
import { useGroupProvider } from './group.js';

const AnimatedView = createAnimatedComponent('View');

function hostProps(props, handlers, extraOmit) {
  const rest = omit(props, PRESS_PROPS, extraOmit);
  Object.assign(rest, handlers);
  // Interactive elements are focus targets for keyboard / gamepad navigation.
  if (rest.focusable === undefined) rest.focusable = !props.disabled;
  if (props.disabled) {
    rest.disabled = true;  // native: not pressable, not focusable
    rest.accessibilityState = Object.assign({ disabled: true }, props.accessibilityState);
  }
  return rest;
}

const PRESSABLE_OMIT = ['children', 'style'];

export function Pressable(props) {
  const { children, style } = props;
  // Only re-render on interaction state when something depends on it.
  const track = typeof style === 'function' || typeof children === 'function' || props.testOnly_pressed !== undefined;
  const [handlers, state] = usePress(props, track);
  const s = props.testOnly_pressed ? Object.assign({}, state, { pressed: true }) : state;
  const rest = hostProps(props, handlers, PRESSABLE_OMIT);
  rest.style = typeof style === 'function' ? style(s) : style;
  // group: descendants' useGroupState() reads this element's interaction.
  const g = useGroupProvider(props.group, props.interaction, typeof children === 'function' ? children(s) : children);
  if (props.group) rest.interaction = g.ix;
  rest.children = g.children;
  return jsx('View', rest);  // rest is ours: no copy (createElement would)
}
Pressable.displayName = 'Pressable';

const OPACITY_OMIT = ['children', 'style', 'activeOpacity'];

export function TouchableOpacity(props) {
  const { activeOpacity = 0.2, style, children } = props;
  const flat = flatten(style);
  const base = flat && flat.opacity != null ? flat.opacity : 1;
  const opacityRef = useRef(null);
  if (!opacityRef.current) opacityRef.current = new AnimatedValue(base);
  const opacity = opacityRef.current;
  const to = (value, duration) => timing(opacity, { toValue: value, duration, easing: Easing.inOut(Easing.quad) }).start();
  const p = Object.assign({}, props, {
    onPressIn(e) {
      to(activeOpacity, e && e.type === 'pointerdown' && e.pointerType === 'touch' ? 0 : 150);
      if (props.onPressIn) props.onPressIn(e);
    },
    onPressOut(e) {
      to(base, 250);
      if (props.onPressOut) props.onPressOut(e);
    },
  });
  const [handlers] = usePress(p, false);
  useEffect(() => {
    // Style opacity changed while idle: follow it.
    opacity.setValue(base);
  }, [base, opacity]);
  const rest = hostProps(p, handlers, OPACITY_OMIT);
  rest.style = [style, { opacity }];
  return h(AnimatedView, rest, children);
}
TouchableOpacity.displayName = 'TouchableOpacity';

const HIGHLIGHT_OMIT = ['children', 'style', 'activeOpacity', 'underlayColor', 'onShowUnderlay', 'onHideUnderlay'];

export function TouchableHighlight(props) {
  const { activeOpacity = 0.85, underlayColor = 'black', style, children } = props;
  const p = Object.assign({}, props, {
    onPressIn(e) {
      if (props.onShowUnderlay) props.onShowUnderlay();
      if (props.onPressIn) props.onPressIn(e);
    },
    onPressOut(e) {
      if (props.onHideUnderlay) props.onHideUnderlay();
      if (props.onPressOut) props.onPressOut(e);
    },
  });
  const [handlers, state] = usePress(p, true);
  const rest = hostProps(p, handlers, HIGHLIGHT_OMIT);
  rest.style = state.pressed ? [style, { backgroundColor: underlayColor }] : style;
  let child = Children.count(children) === 1 ? Children.only(children) : children;
  if (state.pressed && isValidElement(child)) child = cloneElement(child, { style: [child.props.style, { opacity: activeOpacity }] });
  return h('View', rest, child);
}
TouchableHighlight.displayName = 'TouchableHighlight';

const WITHOUT_FEEDBACK_OMIT = ['children'];

export function TouchableWithoutFeedback(props) {
  const [handlers] = usePress(props, false);
  const rest = hostProps(props, handlers, WITHOUT_FEEDBACK_OMIT);
  const child = Children.count(props.children) === 1 ? Children.only(props.children) : null;
  if (!isValidElement(child)) return h('View', rest, props.children);
  // RN clones the single child with the responder props.
  delete rest.ref;
  return cloneElement(child, rest);
}
TouchableWithoutFeedback.displayName = 'TouchableWithoutFeedback';

export function TouchableNativeFeedback(props) {
  return h(TouchableWithoutFeedback, props);
}
TouchableNativeFeedback.displayName = 'TouchableNativeFeedback';
TouchableNativeFeedback.SelectableBackground = () => ({ type: 'ThemeAttrAndroid', attribute: 'selectableItemBackground' });
TouchableNativeFeedback.SelectableBackgroundBorderless = () => ({ type: 'ThemeAttrAndroid', attribute: 'selectableItemBackgroundBorderless' });
TouchableNativeFeedback.Ripple = (color, borderless) => ({ type: 'RippleAndroid', color, borderless });
TouchableNativeFeedback.canUseNativeForeground = () => false;

// Same look as react-native-web's Button (the reference renderer).
const buttonStyles = StyleSheet.create({
  button: { backgroundColor: '#2196F3', borderRadius: 2 },
  text: { color: '#fff', fontWeight: '500', padding: 8, textAlign: 'center', textTransform: 'uppercase' },
  buttonDisabled: { backgroundColor: '#dfdfdf' },
  textDisabled: { color: '#a1a1a1' },
});

export function Button(props) {
  const { title, color, disabled, onPress, testID, accessibilityLabel, nativeID, hasTVPreferredFocus, focusable, autoFocus } = props;
  return h(
    TouchableOpacity,
    {
      accessibilityLabel,
      accessibilityRole: 'button',
      disabled,
      focusable,
      autoFocus,
      hasTVPreferredFocus,
      nativeID,
      onPress,
      ref: props.ref,
      style: [buttonStyles.button, color && { backgroundColor: color }, disabled && buttonStyles.buttonDisabled],
      testID,
    },
    h(Text, { style: [buttonStyles.text, disabled && buttonStyles.textDisabled] }, title),
  );
}
Button.displayName = 'Button';
