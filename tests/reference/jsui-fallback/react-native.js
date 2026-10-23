// Minimal `react-native` for JSUI, used by the reference harness ONLY while
// @orktes/jsui-react (packages/react) is not available. It drives the
// __jsui_native bridge from docs/ARCHITECTURE.md with react-reconciler so the
// Chrome-vs-JSUI pipeline can run end to end. No events, no animation:
// static rendering of View/Text/Image/ImageBackground/ScrollView.
import React from 'react';
import Reconciler from 'react-reconciler';
import { DefaultEventPriority } from 'react-reconciler/constants';

const native = globalThis.__jsui_native;

function flatten(style, out = {}) {
  if (!style) return out;
  if (Array.isArray(style)) {
    for (const s of style) flatten(s, out);
    return out;
  }
  return Object.assign(out, style);
}

function sourceUri(source) {
  if (source == null) return undefined;
  if (typeof source === 'string') return source;
  if (Array.isArray(source)) return sourceUri(source[0]);
  return source.uri;
}

function nativeProps(type, props) {
  const out = {};
  for (const k in props) {
    if (k === 'children' || k === 'style' || typeof props[k] === 'function') continue;
    out[k] = props[k];
  }
  out.style = flatten(props.style);
  if (type === 'Image') {
    const uri = sourceUri(props.source);
    out.source = uri ? { uri } : undefined;
    out.src = uri;
    if (props.resizeMode && !out.style.resizeMode) out.style.resizeMode = props.resizeMode;
  }
  return out;
}

let currentPriority = 0;
const hostConfig = {
  supportsMutation: true,
  supportsPersistence: false,
  supportsHydration: false,
  isPrimaryRenderer: true,
  supportsMicrotasks: true,
  scheduleMicrotask: (fn) => Promise.resolve().then(fn),
  scheduleTimeout: setTimeout,
  cancelTimeout: clearTimeout,
  noTimeout: -1,
  getRootHostContext: () => ({ inText: false }),
  getChildHostContext: (parent, type) => {
    const inText = type === 'Text';
    return parent.inText === inText ? parent : { inText };
  },
  getPublicInstance: (i) => i,
  prepareForCommit: () => null,
  resetAfterCommit: () => {},
  preparePortalMount: () => {},
  createInstance(type, props) {
    const id = native.createNode(type);
    native.setProps(id, nativeProps(type, props));
    return { id, type };
  },
  createTextInstance(text) {
    const id = native.createNode('RawText');
    native.setText(id, String(text));
    return { id, type: 'RawText' };
  },
  appendInitialChild: (p, c) => native.appendChild(p.id, c.id),
  appendChild: (p, c) => native.appendChild(p.id, c.id),
  appendChildToContainer: (c, child) => native.appendChild(c.id, child.id),
  insertBefore: (p, c, b) => native.insertBefore(p.id, c.id, b.id),
  insertInContainerBefore: (c, child, b) => native.insertBefore(c.id, child.id, b.id),
  removeChild: (p, c) => {
    native.removeChild(p.id, c.id);
    native.destroyNode(c.id);
  },
  removeChildFromContainer: (c, child) => {
    native.removeChild(c.id, child.id);
    native.destroyNode(child.id);
  },
  finalizeInitialChildren: () => false,
  shouldSetTextContent: () => false,
  commitUpdate: (inst, type, oldProps, newProps) => native.setProps(inst.id, nativeProps(type, newProps)),
  commitTextUpdate: (inst, oldText, newText) => native.setText(inst.id, String(newText)),
  commitMount: () => {},
  resetTextContent: () => {},
  clearContainer: () => {},
  hideInstance: () => {},
  unhideInstance: () => {},
  hideTextInstance: () => {},
  unhideTextInstance: () => {},
  detachDeletedInstance: () => {},
  getInstanceFromNode: () => null,
  beforeActiveInstanceBlur: () => {},
  afterActiveInstanceBlur: () => {},
  prepareScopeUpdate: () => {},
  getInstanceFromScope: () => null,
  setCurrentUpdatePriority: (p) => (currentPriority = p),
  getCurrentUpdatePriority: () => currentPriority,
  resolveUpdatePriority: () => currentPriority || DefaultEventPriority,
  getCurrentEventPriority: () => DefaultEventPriority,
  maySuspendCommit: () => false,
  preloadInstance: () => true,
  startSuspendingCommit: () => {},
  suspendInstance: () => {},
  waitForCommitToBeReady: () => null,
  NotPendingTransition: null,
  HostTransitionContext: React.createContext(null),
  resetFormInstance: () => {},
  requestPostPaintCallback: () => {},
  shouldAttemptEagerTransition: () => false,
  trackSchedulerEvent: () => {},
  resolveEventType: () => null,
  resolveEventTimeStamp: () => -1.1,
};

const reconciler = Reconciler(hostConfig);

const h = React.createElement;
const host = (type) => {
  const C = React.forwardRef((props, ref) => h(type, { ...props, ref }));
  C.displayName = type;
  return C;
};

export const View = host('View');
export const Text = host('Text');
export const Image = host('Image');
export const ScrollView = host('ScrollView');
export const TextInput = host('TextInput');
export const Pressable = React.forwardRef(({ children, style, ...rest }, ref) =>
  h('View', { ...rest, ref, style: typeof style === 'function' ? style({ pressed: false, hovered: false, focused: false }) : style },
    typeof children === 'function' ? children({ pressed: false, hovered: false, focused: false }) : children),
);
export const TouchableOpacity = Pressable;
export const SafeAreaView = View;

export function ImageBackground({ children, style, imageStyle, source, resizeMode, ...rest }) {
  return h('View', { ...rest, style },
    h('Image', { source, resizeMode, style: [{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }, imageStyle] }),
    children);
}

export const StyleSheet = {
  create: (s) => s,
  flatten: (s) => flatten(s),
  compose: (a, b) => (a && b ? [a, b] : a || b),
  absoluteFill: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0 },
  absoluteFillObject: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0 },
  hairlineWidth: 1,
};

export const Platform = { OS: 'jsui', select: (o) => ('jsui' in o ? o.jsui : 'native' in o ? o.native : o.default) };
export const Dimensions = { get: () => ({ width: 0, height: 0, scale: 1, fontScale: 1 }) };

const registry = new Map();
export const AppRegistry = {
  registerComponent(name, getComponent) {
    registry.set(name, getComponent);
    return name;
  },
  runApplication(name) {
    const App = registry.get(name)();
    const container = { id: native.rootId };
    const noop = () => {};
    const root = reconciler.createContainer(container, 0, null, false, null, '', console.error, console.error, console.error, noop, null);
    reconciler.updateContainerSync(h(App), root, null, null);
    reconciler.flushSyncWork();
  },
};

export default { View, Text, Image, ScrollView, TextInput, Pressable, TouchableOpacity, ImageBackground, StyleSheet, Platform, Dimensions, AppRegistry };
