// Core host-backed components: View, Text, Image, ImageBackground,
// ScrollView, TextInput.

import { createElement as h, useImperativeHandle, useRef, useState, useLayoutEffect, createContext, useContext } from 'react';
import { flatten } from '../StyleSheet.js';
import { native, warnOnce } from '../native.js';
import { usePress, PRESS_PROPS, omit } from './usePress.js';
import { createSyntheticEvent } from '../events.js';

// View is the host element itself: no wrapper fiber, which halves the fiber
// count of typical trees. `<View>` therefore costs exactly one host node.
export const View = 'View';

// --- Text ------------------------------------------------------------------

/** True inside a <Text> (RN's TextAncestorContext). */
export const TextAncestorContext = createContext(false);

const TEXT_PRESS = ['onPress', 'onPressIn', 'onPressOut', 'onLongPress'];

function PressableText(props) {
  const [handlers] = usePress(props, false);
  const rest = omit(props, PRESS_PROPS);
  Object.assign(rest, handlers);
  if (rest.focusable === undefined) rest.focusable = !props.disabled;
  return h('Text', rest);
}

export function Text(props) {
  for (let i = 0; i < TEXT_PRESS.length; i++) {
    if (props[TEXT_PRESS[i]]) return h(PressableText, props);
  }
  return h('Text', props);
}
Text.displayName = 'Text';

// --- Image -------------------------------------------------------------------

// Asset sizes registered by @jsui/cli for imported images (url -> {width, height}).
function assetInfo(url) {
  const reg = globalThis.__jsui_assets;
  return reg && reg[url];
}

const numericAssets = [];
/** RN AssetRegistry compatibility: registerAsset({uri|httpServerLocation, width, height}) -> number. */
export function registerAsset(asset) {
  numericAssets.push(asset);
  return numericAssets.length;
}

/** Normalizes RN image sources to {uri, width?, height?, scale?} or null. */
export function resolveAssetSource(source) {
  if (source == null) return null;
  if (typeof source === 'number') {
    const a = numericAssets[source - 1];
    if (!a) return null;
    return { uri: a.uri || a.httpServerLocation, width: a.width, height: a.height, scale: a.scale || 1 };
  }
  if (typeof source === 'string') {
    const info = assetInfo(source);
    return info ? { uri: source, width: info.width, height: info.height, scale: info.scale || 1 } : { uri: source };
  }
  if (Array.isArray(source)) {
    // Multiple resolutions: pick the smallest one that covers the pixel ratio.
    if (source.length === 0) return null;
    return resolveAssetSource(source[0]);
  }
  if (typeof source === 'object') {
    if (source.uri == null && source.testUri == null) return null;
    const uri = source.uri != null ? source.uri : source.testUri;
    const info = typeof uri === 'string' ? assetInfo(uri) : null;
    if (info && source.width == null && source.height == null) return Object.assign({}, source, { uri, width: info.width, height: info.height });
    return source;
  }
  return null;
}

function ImageImpl(props) {
  const { source, src, defaultSource, style, resizeMode, tintColor, onLoad, onLoadStart, onLoadEnd, onError, width, height, ...rest } = props;
  let resolved = resolveAssetSource(source != null ? source : src != null ? { uri: src } : defaultSource);
  const flat = flatten(style);
  // Like react-native-web (the reference): only dimensions written by the app
  // (width/height props or source.width/height) become style. Bundled asset
  // sizes are passed to native as an intrinsic size that Yoga uses only when
  // layout leaves the image unsized, so flex/stretch styles still win.
  let sized = flat;
  const explicit = source && typeof source === 'object' && !Array.isArray(source) ? source : null;
  const w = width != null ? width : explicit && explicit.width;
  const hgt = height != null ? height : explicit && explicit.height;
  if ((w != null && (!flat || flat.width === undefined)) || (hgt != null && (!flat || flat.height === undefined))) {
    sized = Object.assign({}, w != null ? { width: w } : null, hgt != null ? { height: hgt } : null, flat);
  }
  if (resizeMode && (!sized || sized.resizeMode === undefined)) sized = Object.assign({}, sized, { resizeMode });
  if (tintColor && (!sized || sized.tintColor === undefined)) sized = Object.assign({}, sized, { tintColor });
  rest.style = sized;
  rest.source = resolved ? { uri: resolved.uri, width: resolved.width, height: resolved.height } : null;
  if (resizeMode) rest.resizeMode = resizeMode;
  const started = useRef(null);
  useLayoutEffect(() => {
    const uri = resolved && resolved.uri;
    if (started.current !== uri) {
      started.current = uri;
      if (onLoadStart) onLoadStart(createSyntheticEvent('loadstart', {}, null));
    }
  });
  if (onLoad || onLoadEnd) {
    rest.onLoad = (e) => {
      if (onLoad) onLoad(e);
      if (onLoadEnd) onLoadEnd(e);
    };
  }
  if (onError || onLoadEnd) {
    rest.onError = (e) => {
      if (onError) onError(e);
      if (onLoadEnd) onLoadEnd(e);
    };
  }
  return h('Image', rest);
}

export const Image = ImageImpl;
Image.displayName = 'Image';
Image.resolveAssetSource = resolveAssetSource;
Image.getSize = (uri, success, failure) => {
  const info = assetInfo(uri);
  if (info) {
    if (success) success(info.width, info.height);
    return Promise.resolve(info);
  }
  if (typeof native.getImageSize === 'function') {
    return Promise.resolve(native.getImageSize(uri)).then(
      (s) => {
        if (success) success(s.width, s.height);
        return s;
      },
      (e) => {
        if (failure) failure(e);
        throw e;
      },
    );
  }
  const err = new Error('Image.getSize: size unknown for ' + uri);
  if (failure) failure(err);
  return Promise.reject(err);
};
Image.getSizeWithHeaders = (uri, _headers, success, failure) => Image.getSize(uri, success, failure);
Image.prefetch = (uri) => (typeof native.prefetchImage === 'function' ? Promise.resolve(native.prefetchImage(uri)) : Promise.resolve(true));
Image.prefetchWithMetadata = Image.prefetch;
Image.queryCache = () => Promise.resolve({});
Image.abortPrefetch = () => {};

export function ImageBackground(props) {
  const { children, style, imageStyle, imageRef, importantForAccessibility, ref, ...rest } = props;
  const flat = flatten(style) || {};
  return h(
    'View',
    { style, ref },
    h(Image, Object.assign({}, rest, { ref: imageRef, style: [{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, width: flat.width, height: flat.height }, imageStyle] })),
    children,
  );
}

// --- ScrollView --------------------------------------------------------------

const scrollBase = { flexGrow: 1, flexShrink: 1, flexDirection: 'column', overflow: 'scroll' };
const scrollBaseH = { flexGrow: 1, flexShrink: 1, flexDirection: 'row', overflow: 'scroll' };
const contentH = { flexDirection: 'row' };

const SCROLL_IGNORED = [
  'contentContainerStyle',
  'onContentSizeChange',
  'refreshControl',
  'keyboardShouldPersistTaps',
  'keyboardDismissMode',
  'stickyHeaderIndices',
  'StickyHeaderComponent',
  'invertStickyHeaders',
  'children',
  'centerContent',
];

export function ScrollView(props) {
  const { children, contentContainerStyle, horizontal, onContentSizeChange, refreshControl, centerContent } = props;
  const rest = omit(props, SCROLL_IGNORED);
  rest.horizontal = !!horizontal;
  rest.style = [horizontal ? scrollBaseH : scrollBase, props.style];
  const content = {
    style: [horizontal && contentH, centerContent && { flexGrow: 1, justifyContent: 'center' }, contentContainerStyle],
    collapsable: false,
  };
  if (onContentSizeChange) content.onLayout = (e) => onContentSizeChange(e.nativeEvent.layout.width, e.nativeEvent.layout.height);
  return h('ScrollView', rest, refreshControl || null, h('View', content, children));
}
ScrollView.displayName = 'ScrollView';

// --- TextInput -----------------------------------------------------------------
//
// With the JSUI runtime (native.textEditing) the text is edited natively,
// like a React Native text field: keys, text, clicks, drag / double-click
// selection, clipboard and undo edit the native copy, which reports
// 'change' ({text, eventCount}) and 'selectionchange' ({selection}). The
// host `value` prop plus `mostRecentEventCount` reconcile it with React
// (native ignores renders that have not seen its latest edit), so
// controlled inputs can accept, transform or reject edits. The controlled
// `selection` prop and ref.setSelection() use native.setTextInputSelection.
// Without native editing (e.g. a test bridge) typing appends at the end.

function deleteLastCodePoint(s) {
  if (!s) return s;
  const cps = Array.from(s);
  cps.pop();
  return cps.join('');
}

const TEXTINPUT_OMIT = [
  'onChangeText',
  'onChange',
  'onSubmitEditing',
  'onKeyPress',
  'onEndEditing',
  'defaultValue',
  'blurOnSubmit',
  'submitBehavior',
  'clearTextOnFocus',
  'onSelectionChange',
  'selection',
  'onFocus',
  'onBlur',
  'onKeyDown',
  'onTextInput',
  'onPointerDown',
];

let focusedInput = null;

function selectionOf(sel) {
  if (!sel || typeof sel.start !== 'number') return null;
  return { start: sel.start, end: typeof sel.end === 'number' ? sel.end : sel.start };
}

/** What Enter does: 'newline', 'submit' or 'blurAndSubmit' (RN submitBehavior). */
function submitBehaviorOf(p) {
  if (p.submitBehavior === 'submit' || p.submitBehavior === 'blurAndSubmit' || p.submitBehavior === 'newline') {
    return p.multiline || p.submitBehavior !== 'newline' ? p.submitBehavior : 'blurAndSubmit';
  }
  if (p.multiline) return p.blurOnSubmit === true ? 'blurAndSubmit' : 'newline';
  return p.blurOnSubmit === false ? 'submit' : 'blurAndSubmit';
}

export function TextInput(props) {
  const { value, defaultValue, editable = true, readOnly, multiline, maxLength, secureTextEntry } = props;
  const nativeEditing = native.textEditing === true;
  const [inner, setInner] = useState(defaultValue != null ? String(defaultValue) : '');
  const [, setRevision] = useState(0);
  const controlled = value !== undefined && value !== null;
  const current = controlled ? String(value) : inner;
  const hostRef = useRef(null);
  const focusedRef = useRef(false);
  const eventCount = useRef(0);
  const nativeSelection = useRef(null);
  const latest = useRef(null);
  latest.current = { props, current, controlled };
  const canEdit = editable && !readOnly;

  const target = () => hostRef.current && hostRef.current.id;
  const report = (text) => {
    const p = latest.current.props;
    if (p.onChange) p.onChange({ nativeEvent: { text, eventCount: eventCount.current, target: target() }, target: target() });
    if (p.onChangeText) p.onChangeText(text);
  };
  // A JS-side edit (clear(), clearTextOnFocus, the non-native fallback).
  const commit = (text) => {
    if (maxLength != null && Array.from(text).length > maxLength) text = Array.from(text).slice(0, maxLength).join('');
    if (text === latest.current.current) return;
    if (!latest.current.controlled) setInner(text);
    // Keep the displayed value in sync immediately, also for controlled
    // inputs whose owner re-renders asynchronously.
    latest.current.current = text;
    report(text);
  };
  const setSelection = (start, end) => {
    const inst = hostRef.current;
    if (!inst || typeof native.setTextInputSelection !== 'function') return;
    const sel = { start: +start || 0, end: end == null ? +start || 0 : +end || 0 };
    nativeSelection.current = sel;
    native.setTextInputSelection(inst.id, sel.start, sel.end);
  };

  useImperativeHandle(props.ref, () => {
    const inst = hostRef.current;
    return Object.assign(Object.create(inst || null), {
      focus: () => inst && inst.focus(),
      blur: () => inst && inst.blur(),
      clear: () => commit(''),
      isFocused: () => focusedRef.current,
      setNativeProps: (np) => inst && inst.setNativeProps(np),
      getNativeRef: () => inst,
      setSelection,
    });
  });

  // Controlled selection: re-applied whenever native reports a different one.
  const selection = selectionOf(props.selection);
  useLayoutEffect(() => {
    if (!nativeEditing || !selection) return;
    const s = nativeSelection.current;
    if (!s || s.start !== selection.start || s.end !== selection.end) setSelection(selection.start, selection.end);
  });

  const rest = omit(props, TEXTINPUT_OMIT);
  rest.value = current;
  rest.ref = hostRef;
  rest.editable = canEdit;
  if (secureTextEntry) rest.secureTextEntry = true;
  if (rest.focusable === undefined) rest.focusable = canEdit;
  if (nativeEditing) {
    rest.mostRecentEventCount = eventCount.current;
    rest.onChange = (e) => {
      const ne = e.nativeEvent;
      const text = ne.text != null ? String(ne.text) : '';
      if (typeof ne.eventCount === 'number') eventCount.current = ne.eventCount;
      latest.current.current = text;
      if (!latest.current.controlled) setInner(text);
      report(text);
      // Re-render with the new event count: native then adopts the owner's
      // value (which may differ from what was typed, or revert it).
      setRevision((r) => r + 1);
    };
    rest.onSelectionChange = (e) => {
      const sel = selectionOf(e.nativeEvent.selection) || { start: 0, end: 0 };
      nativeSelection.current = sel;
      const p = latest.current.props;
      if (p.onSelectionChange) p.onSelectionChange({ nativeEvent: { selection: { start: sel.start, end: sel.end }, target: target() }, target: target() });
      if (selectionOf(p.selection)) setRevision((r) => r + 1);
    };
  }
  rest.onPointerDown = (e) => {
    if (props.onPointerDown) props.onPointerDown(e);
    if (!nativeEditing && canEdit && hostRef.current) hostRef.current.focus();
  };
  rest.onFocus = (e) => {
    focusedRef.current = true;
    focusedInput = hostRef.current;
    const p = latest.current.props;
    if (p.clearTextOnFocus) commit('');
    if (p.onFocus) p.onFocus(e);
  };
  rest.onBlur = (e) => {
    focusedRef.current = false;
    if (focusedInput === hostRef.current) focusedInput = null;
    const p = latest.current.props;
    if (p.onEndEditing) p.onEndEditing({ nativeEvent: { text: latest.current.current, target: target() } });
    if (p.onBlur) p.onBlur(e);
  };
  rest.onTextInput = (e) => {
    const p = latest.current.props;
    if (p.onTextInput) p.onTextInput(e);
    if (nativeEditing || !canEdit) return;
    let text = e.nativeEvent.text || '';
    if (!multiline) text = text.replace(/[\r\n]/g, '');
    if (!text) return;
    e.stopPropagation();
    commit(latest.current.current + text);
  };
  rest.onKeyDown = (e) => {
    const p = latest.current.props;
    if (p.onKeyDown) p.onKeyDown(e);
    if (p.onKeyPress) p.onKeyPress({ nativeEvent: { key: e.key, target: target() }, key: e.key, preventDefault: () => e.preventDefault() });
    if (e.isDefaultPrevented()) return;
    const k = e.key;
    if (k === 'Enter') {
      const behavior = submitBehaviorOf(p);
      e.stopPropagation();
      if (behavior === 'newline') {
        if (!nativeEditing && canEdit) {
          e.preventDefault();
          commit(latest.current.current + '\n');
        }
        return;  // native inserts the newline
      }
      e.preventDefault();
      if (p.onSubmitEditing) p.onSubmitEditing({ nativeEvent: { text: latest.current.current, target: target() } });
      if (behavior === 'blurAndSubmit' && hostRef.current) hostRef.current.blur();
    } else if (k === 'Escape') {
      e.stopPropagation();
      if (hostRef.current) hostRef.current.blur();
    } else if (!nativeEditing && canEdit && k === 'Backspace') {
      e.preventDefault();
      e.stopPropagation();
      commit(deleteLastCodePoint(latest.current.current));
    }
  };
  return h('TextInput', rest);
}
TextInput.displayName = 'TextInput';
TextInput.State = {
  currentlyFocusedInput: () => focusedInput,
  currentlyFocusedField: () => (focusedInput ? focusedInput.id : null),
  focusTextInput: (r) => r && r.focus && r.focus(),
  blurTextInput: (r) => r && r.blur && r.blur(),
};

/** Text.contextType-like hook: inside a <Text>? */
export function useTextAncestor() {
  return useContext(TextAncestorContext);
}

export function unsupportedComponent(name) {
  function Unsupported(props) {
    warnOnce('component:' + name, `[jsui] <${name}> is not supported; rendering a View`);
    return h('View', { style: props.style }, props.children);
  }
  Unsupported.displayName = name;
  return Unsupported;
}
