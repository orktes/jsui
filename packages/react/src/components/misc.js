// ActivityIndicator, Switch, Modal, StatusBar and the trivial containers.

import { createElement as h, useEffect, useLayoutEffect, useRef } from 'react';
import { Pressable } from './pressables.js';
import { AnimatedValue } from '../animated/nodes.js';
import { timing, loop } from '../animated/animations.js';
import { Easing } from '../animated/Easing.js';
import { createAnimatedComponent } from '../animated/createAnimatedComponent.js';
import { createPortal } from '../renderer.js';
import { addGlobalListener } from '../events.js';

const AnimatedView = createAnimatedComponent('View');

// --- ActivityIndicator ---------------------------------------------------------

const SIZES = { small: 20, large: 36 };

export function ActivityIndicator(props) {
  const { animating = true, color = '#1976D2', hidesWhenStopped = true, size = 'small', style, ...rest } = props;
  const px = typeof size === 'number' ? size : SIZES[size] || 20;
  const spin = useRef(null);
  if (!spin.current) spin.current = new AnimatedValue(0);
  useEffect(() => {
    if (!animating) return undefined;
    // One shared rAF loop drives this; React does not re-render per frame.
    const anim = loop(timing(spin.current, { toValue: 1, duration: 750, easing: Easing.linear }));
    anim.start();
    return () => anim.stop();
  }, [animating]);
  const stroke = Math.max(1, Math.round(px / 8));
  const ring = { position: 'absolute', left: 0, top: 0, width: px, height: px, borderRadius: px / 2, borderWidth: stroke };
  const rotate = spin.current.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '360deg'] });
  return h(
    'View',
    Object.assign({ accessibilityRole: 'progressbar' }, rest, { style: [{ alignItems: 'center', justifyContent: 'center' }, style] }),
    h(
      'View',
      { style: { width: px, height: px, opacity: !animating && hidesWhenStopped ? 0 : 1 } },
      h('View', { style: [ring, { borderColor: color, opacity: 0.2 }] }),
      h(AnimatedView, {
        style: [ring, { borderColor: 'transparent', borderTopColor: color, borderRightColor: color, transform: [{ rotate }] }],
      }),
    ),
  );
}
ActivityIndicator.displayName = 'ActivityIndicator';

// --- Switch (react-native-web look) ------------------------------------------

export function Switch(props) {
  const {
    value = false,
    onValueChange,
    onChange,
    disabled,
    trackColor,
    thumbColor,
    activeThumbColor,
    activeTrackColor,
    ios_backgroundColor,
    style,
    ...rest
  } = props;
  const onTrack = (trackColor && trackColor.true) || activeTrackColor || '#A3D3CF';
  const offTrack = (trackColor && trackColor.false) || ios_backgroundColor || '#939393';
  const thumb = value ? thumbColor || activeThumbColor || '#009688' : thumbColor || '#FAFAFA';
  return h(
    Pressable,
    Object.assign({ accessibilityRole: 'switch' }, rest, {
      disabled,
      accessibilityState: { checked: !!value, disabled: !!disabled },
      onPress: (e) => {
        if (onChange) onChange(Object.assign(e, { nativeEvent: Object.assign(e.nativeEvent || {}, { value: !value }) }));
        if (onValueChange) onValueChange(!value);
      },
      style: [{ height: 20, width: 40, justifyContent: 'center' }, style],
    }),
    h('View', {
      style: {
        position: 'absolute',
        left: 0,
        right: 0,
        top: 3,
        bottom: 3,
        borderRadius: 7,
        backgroundColor: disabled ? '#D5D5D5' : value ? onTrack : offTrack,
      },
    }),
    h('View', {
      style: {
        width: 20,
        height: 20,
        borderRadius: 10,
        alignSelf: 'flex-start',
        marginLeft: value ? 20 : 0,
        backgroundColor: disabled ? '#BDBDBD' : thumb,
        boxShadow: '0px 1px 3px rgba(0, 0, 0, 0.5)',
      },
    }),
  );
}
Switch.displayName = 'Switch';

// --- Modal ---------------------------------------------------------------------

// Stack of visible modals: Escape / gamepad B / BackHandler go to the top one.
const modalStack = [];
let removeKeyListener = null;
let removePadListener = null;

function requestCloseTop(e) {
  const top = modalStack[modalStack.length - 1];
  if (!top || !top.current.onRequestClose) return;
  top.current.onRequestClose();
  if (e) e.preventDefault();
}

function ensureListeners() {
  if (removeKeyListener) return;
  removeKeyListener = addGlobalListener('keydown', (e) => {
    if (!e.isDefaultPrevented() && (e.key === 'Escape' || e.key === 'GoBack' || e.key === 'BrowserBack')) requestCloseTop(e);
  });
  // Standard mapping: button 1 is B / Circle.
  removePadListener = addGlobalListener('gamepadbutton', (e) => {
    if (!e.isDefaultPrevented() && e.buttonIndex === 1 && e.pressed) requestCloseTop(e);
  });
}

function releaseListeners() {
  if (modalStack.length || !removeKeyListener) return;
  removeKeyListener();
  removePadListener();
  removeKeyListener = removePadListener = null;
}

/** Called by BackHandler before its own listeners: true if a modal consumed it. */
export function modalHandlesBack() {
  const top = modalStack[modalStack.length - 1];
  if (!top || !top.current.onRequestClose) return false;
  top.current.onRequestClose();
  return true;
}

const modalFill = { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, zIndex: 10000 };
const modalFillOpaque = Object.assign({}, modalFill, { backgroundColor: 'white' });

function ModalContent(props) {
  const latest = useRef(props);
  latest.current = props;
  useLayoutEffect(() => {
    modalStack.push(latest);
    ensureListeners();
    if (latest.current.onShow) latest.current.onShow();
    return () => {
      const i = modalStack.indexOf(latest);
      if (i >= 0) modalStack.splice(i, 1);
      releaseListeners();
      if (latest.current.onDismiss) latest.current.onDismiss();
    };
  }, []);
  return createPortal(
    h(
      'View',
      {
        style: [props.transparent ? modalFill : modalFillOpaque, props.style],
        // Native focus scope: Tab / gamepad navigation stays inside the modal.
        accessibilityViewIsModal: true,
        pointerEvents: 'auto',
      },
      props.children,
    ),
  );
}

export function Modal(props) {
  const { visible = true } = props;
  if (!visible) return null;
  return h(ModalContent, props);
}
Modal.displayName = 'Modal';

// --- trivial ------------------------------------------------------------------

export function StatusBar() {
  return null;
}
StatusBar.displayName = 'StatusBar';
StatusBar.currentHeight = 0;
StatusBar.setHidden = () => {};
StatusBar.setBarStyle = () => {};
StatusBar.setBackgroundColor = () => {};
StatusBar.setTranslucent = () => {};
StatusBar.setNetworkActivityIndicatorVisible = () => {};
StatusBar.pushStackEntry = (p) => p;
StatusBar.popStackEntry = () => {};
StatusBar.replaceStackEntry = (_e, p) => p;

function passthroughView(name) {
  function C(props) {
    return h('View', props);
  }
  C.displayName = name;
  return C;
}

export const SafeAreaView = passthroughView('SafeAreaView');
export const KeyboardAvoidingView = (props) => {
  const { behavior, keyboardVerticalOffset, contentContainerStyle, enabled, ...rest } = props;
  return h('View', rest);
};
KeyboardAvoidingView.displayName = 'KeyboardAvoidingView';
export const InputAccessoryView = () => null;
export const RefreshControl = () => null;
export const DrawerLayoutAndroid = (props) => {
  const { renderNavigationView, drawerWidth, drawerPosition, drawerLockMode, onDrawerOpen, onDrawerClose, ...rest } = props;
  return h('View', rest);
};
export function ProgressBarAndroid(props) {
  const { progress = 0, indeterminate, color = '#1976D2', style } = props;
  if (indeterminate !== false && props.progress === undefined) return h(ActivityIndicator, { color, style });
  return h(
    'View',
    { style: [{ height: 4, backgroundColor: 'rgba(0,0,0,0.1)', overflow: 'hidden' }, style] },
    h('View', { style: { width: Math.max(0, Math.min(1, progress)) * 100 + '%', height: '100%', backgroundColor: color } }),
  );
}
