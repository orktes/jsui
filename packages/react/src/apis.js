// Non-component React Native APIs.

import { Component, createElement as h, useContext, useEffect, useState, useSyncExternalStore } from 'react';
import { native, queueMicrotask, readViewport, warnOnce } from './native.js';
import { LayoutAnimation } from './layoutAnimation.js';
import { addGlobalListener } from './events.js';
import {
  createPortal,
  flushSync,
  forgetContainer,
  render as renderRoot,
  renderBatch,
  reportToRuntime,
  rootContainer,
  unmount as unmountRoot,
  RootTagContext,
} from './renderer.js';
import { modalHandlesBack } from './components/misc.js';

// --- Platform ------------------------------------------------------------------

export const Platform = {
  OS: 'jsui',
  Version: 1,
  isTV: false,
  isTesting: false,
  isPad: false,
  isVision: false,
  constants: { reactNativeVersion: { major: 0, minor: 81, patch: 0 }, isTesting: false, osVersion: '1', systemName: 'jsui' },
  // jsui first, then RN's generic 'native', then 'default'.
  select(spec) {
    if (!spec) return undefined;
    if ('jsui' in spec) return spec.jsui;
    if ('native' in spec) return spec.native;
    return spec.default;
  },
};

// --- Dimensions ------------------------------------------------------------------

let viewport = readViewport();
let windowDims = null;
const dimListeners = new Set();

function dims() {
  if (!windowDims) windowDims = Object.freeze({ width: viewport.width, height: viewport.height, scale: viewport.scale, fontScale: 1 });
  return windowDims;
}

// Surfaces other than the main one (jsui_surface_create): their own sizes
// and the useWindowDimensions() subscribers inside them.
const surfaceDims = new Map();
const surfaceDimListeners = new Map(); // rootTag -> Set

function surfaceDimsOf(tag) {
  let d = surfaceDims.get(tag);
  if (!d) {
    const v = typeof native.viewport === 'function' ? native.viewport(tag) : null;
    d = v && v.width >= 0 ? Object.freeze({ width: +v.width, height: +v.height, scale: +(v.scale || 1), fontScale: 1 }) : dims();
    surfaceDims.set(tag, d);
  }
  return d;
}

function onResize(ev) {
  if (ev && ev.rootTag != null && ev.rootTag !== native.rootId) {
    surfaceDims.delete(ev.rootTag);
    const set = surfaceDimListeners.get(ev.rootTag);
    if (set) for (const fn of Array.from(set)) fn();
    return;
  }
  const fresh = ev && ev.width > 0 ? { width: +ev.width, height: +ev.height, scale: +(ev.scale || ev.pixelRatio || viewport.scale) } : readViewport();
  if (fresh.width === viewport.width && fresh.height === viewport.height && fresh.scale === viewport.scale) return;
  viewport = fresh;
  windowDims = null;
  const payload = { window: dims(), screen: dims() };
  for (const fn of Array.from(dimListeners)) fn(payload);
}
addGlobalListener('resize', onResize);

export const Dimensions = {
  get(_which) {
    return dims();
  },
  set() {},
  addEventListener(type, handler) {
    if (type !== 'change') return { remove() {} };
    dimListeners.add(handler);
    return { remove: () => dimListeners.delete(handler) };
  },
  removeEventListener(_type, handler) {
    dimListeners.delete(handler);
  },
  /** Re-reads __jsui_native.viewport (hosts without a resize event can call this). */
  refresh() {
    onResize(null);
  },
};

function subscribeDims(cb) {
  dimListeners.add(cb);
  return () => dimListeners.delete(cb);
}

/** The size of the surface the component renders in (the window for the main one). */
export function useWindowDimensions() {
  const tag = useContext(RootTagContext);
  const [store] = useState(() => surfaceDimsStore(tag));
  return useSyncExternalStore(store.subscribe, store.get, store.get);
}

const mainDimsStore = { subscribe: subscribeDims, get: dims };

// Only the components of the resized surface hear about it.
function surfaceDimsStore(tag) {
  if (tag === native.rootId) return mainDimsStore;
  return {
    subscribe(cb) {
      let set = surfaceDimListeners.get(tag);
      if (!set) surfaceDimListeners.set(tag, (set = new Set()));
      set.add(cb);
      return () => {
        set.delete(cb);
        if (!set.size && surfaceDimListeners.get(tag) === set) surfaceDimListeners.delete(tag);
      };
    },
    get: () => surfaceDimsOf(tag),
  };
}

export const PixelRatio = {
  get: () => viewport.scale,
  getFontScale: () => 1,
  getPixelSizeForLayoutSize: (size) => Math.round(size * viewport.scale),
  roundToNearestPixel: (size) => Math.round(size * viewport.scale) / viewport.scale,
  startDetecting() {},
};

// --- AppRegistry -------------------------------------------------------------------
//
// One bundle can show several root views (React Native's RCTRootView): the
// host creates surfaces by app key (jsui_surface_create) and, once per tick,
// the runtime calls __jsui_runSurfaces([[rootTag, appKey, propsJson], ...])
// with the surfaces that started or got new props. A key registered later
// (after an await) starts the surfaces waiting for it.
//
// The main surface's app has a React root of its own. The other surfaces'
// apps all live in one more root, each portaled into its surface's root
// container: a frame that updates the props of many surfaces is one React
// render and commit, and surfaces whose props did not change are skipped.

const registry = new Map();
let started = false; // the main surface runs an app
let autoStartScheduled = false;
const waiting = new Map(); // rootTag -> {appKey, initialProps}: key not registered yet

// Fills a sized surface; in an auto-sized one Yoga ignores the 0 flex basis
// (no definite size to flex in) and the app takes its content's size.
const appStyle = { flex: 1, pointerEvents: 'box-none' };
export { RootTagContext };

// The host picked the main surface's app (jsui_surface_run on the runtime).
function hostPicksMain() {
  return typeof native.surfaceApp === 'function' && native.surfaceApp(native.rootId) != null;
}

function runSurface(rootTag, appKey, initialProps) {
  if (!registry.has(appKey)) {
    waiting.set(rootTag, { appKey, initialProps });
    return;
  }
  waiting.delete(rootTag);
  AppRegistry.runApplication(appKey, { rootTag, initialProps });
}

// The surfaces' root: rootTag -> {appKey, App, props, gen, root} for every
// surface but the main one, each rendered as a portal (SurfaceRoot).
const SURFACES_ROOT = -1; // a container with no native node: it only holds portals
const surfaceApps = new Map();
let surfacesDirty = false; // a surface was added or removed
let surfacesBatch = 0;
const propsChanged = new Set(); // mounted SurfaceRoots with new props

function renderSurfaces() {
  if (surfacesBatch) {
    surfacesDirty = true;
    return;
  }
  surfacesDirty = false;
  renderRoot(h(SurfaceApps, { tags: Array.from(surfaceApps.keys()) }), { container: SURFACES_ROOT });
}

// New props re-render just that surface, not the list of surfaces.
function updateSurfaces() {
  if (surfacesBatch || !propsChanged.size) return;
  const list = Array.from(propsChanged);
  propsChanged.clear();
  flushSync(() => {
    for (const root of list) root.forceUpdate();
  });
}

function SurfaceApps({ tags }) {
  return tags.map((rootTag) => {
    const a = surfaceApps.get(rootTag);
    return h(SurfaceRoot, { key: rootTag + ':' + a.gen, rootTag });
  });
}

// One surface's app, portaled into its root container, with the surface's
// current app and props (surfaceApps). An app that throws unmounts alone (as
// with a root of its own) and the error goes to the runtime like an uncaught
// one.
class SurfaceRoot extends Component {
  constructor(props) {
    super(props);
    this.state = { failed: false };
  }
  static getDerivedStateFromError() {
    return { failed: true };
  }
  shouldComponentUpdate(_next, state) {
    return state !== this.state; // new props arrive through forceUpdate
  }
  componentDidMount() {
    surfaceApps.get(this.props.rootTag).root = this;
  }
  componentWillUnmount() {
    const a = surfaceApps.get(this.props.rootTag);
    if (a && a.root === this) a.root = null;
    propsChanged.delete(this);
  }
  componentDidCatch(error) {
    reportToRuntime(error);
  }
  render() {
    const { rootTag } = this.props;
    const a = surfaceApps.get(rootTag);
    if (this.state.failed || !a) return null;
    return createPortal(
      h(RootTagContext.Provider, { value: rootTag }, h('View', { style: appStyle }, h(a.App, a.props))),
      rootContainer(rootTag),
    );
  }
}

function componentOf(entry) {
  // Resolved once: a getComponent that returns a new function each call
  // (() => () => <View />) must not remount the app on every props update.
  if (!entry.component) entry.component = entry.getComponent();
  return entry.component;
}

export const AppRegistry = {
  registerComponent(appKey, getComponent) {
    registry.set(appKey, { getComponent });
    for (const [rootTag, w] of Array.from(waiting)) if (w.appKey === appKey) runSurface(rootTag, appKey, w.initialProps);
    // RN native starts the registered app automatically. Do it at the end of
    // bundle evaluation (microtask), so a later explicit runApplication call
    // (react-native-web style) or other registrations still win. The
    // runtime's queueMicrotask, not a promise: on the host JS engine
    // (docs/ARCHITECTURE.md) promise jobs wait for the host's own microtask
    // checkpoint, the runtime's queue drains when the bundle has run.
    if (!autoStartScheduled) {
      autoStartScheduled = true;
      queueMicrotask(() => {
        if (!started && registry.has(appKey) && !hostPicksMain()) AppRegistry.runApplication(appKey, {});
      });
    }
    return appKey;
  },
  registerConfig(configs) {
    for (const c of configs) AppRegistry.registerComponent(c.appKey, c.component);
  },
  registerRunnable(appKey, run) {
    registry.set(appKey, { run });
    return appKey;
  },
  registerSection(appKey, getComponent) {
    AppRegistry.registerComponent(appKey, getComponent);
  },
  getAppKeys: () => Array.from(registry.keys()),
  getRunnable: (appKey) => registry.get(appKey),
  getApplication(appKey, params) {
    return { element: AppRegistry._element(appKey, params || {}) };
  },
  _element(appKey, params) {
    const entry = registry.get(appKey);
    if (!entry || !entry.getComponent) throw new Error(`AppRegistry: no component registered as "${appKey}"`);
    const App = componentOf(entry);
    const rootTag = rootTagOf(params);
    return h(RootTagContext.Provider, { value: rootTag }, h('View', { style: appStyle }, h(App, (params && params.initialProps) || {})));
  },
  /** Renders `appKey` into params.rootTag (default: the main surface) with params.initialProps. */
  runApplication(appKey, params) {
    const entry = registry.get(appKey);
    if (!entry) throw new Error(`AppRegistry: application "${appKey}" has not been registered`);
    const rootTag = rootTagOf(params);
    if (rootTag === native.rootId) started = true;
    if (entry.run) return entry.run({ ...(params || {}), rootTag });
    if (rootTag === native.rootId) {
      renderRoot(AppRegistry._element(appKey, params), { container: rootTag });
      return undefined;
    }
    if (!entry.getComponent) throw new Error(`AppRegistry: no component registered as "${appKey}"`);
    const App = componentOf(entry);
    const props = (params && params.initialProps) || {};
    const prev = surfaceApps.get(rootTag);
    if (prev && prev.appKey === appKey && prev.App === App) {
      // New props keep the app's state.
      prev.props = props;
      if (prev.root) {
        propsChanged.add(prev.root);
        updateSurfaces();
      }
      return undefined;
    }
    // A new app on the surface mounts fresh.
    surfaceApps.set(rootTag, { appKey, App, props, gen: prev ? prev.gen + 1 : 0, root: null });
    renderSurfaces();
    return undefined;
  },
  unmountApplicationComponentAtRootTag(rootTag) {
    const tag = typeof rootTag === 'number' ? rootTag : native.rootId;
    waiting.delete(tag);
    if (surfaceApps.delete(tag)) renderSurfaces();
    else unmountRoot({ container: tag });
  },
  setWrapperComponentProvider() {},
  setComponentProviderInstrumentationHook() {},
  enableArchitectureIndicator() {},
};

// A numeric root tag (a surface), else the main surface (react-native-web
// passes a DOM element here).
function rootTagOf(params) {
  const tag = params && params.rootTag;
  return typeof tag === 'number' ? tag : native.rootId;
}

// Called by the runtime once per tick with the surfaces that started or got
// new props (all of them after a bundle loads): one React pass for all.
globalThis.__jsui_runSurfaces = (list) =>
  renderBatch(() => {
    surfacesBatch++;
    try {
      for (const [rootTag, appKey, propsJson] of list) {
        let props = {};
        try {
          props = typeof propsJson === 'string' ? JSON.parse(propsJson) : propsJson || {};
        } catch (e) {
          console.error(`[jsui] surface "${appKey}": invalid props: ${e && e.message}`);
        }
        try {
          runSurface(rootTag, appKey, props);
        } catch (e) {
          console.error(`[jsui] surface "${appKey}": ${(e && e.stack) || e}`);
        }
      }
    } finally {
      surfacesBatch--;
    }
    if (surfacesDirty) renderSurfaces();
    updateSurfaces();
  });
globalThis.__jsui_stopSurface = (rootTag) => {
  surfaceDims.delete(rootTag);
  surfaceDimListeners.delete(rootTag);
  AppRegistry.unmountApplicationComponentAtRootTag(rootTag);
  forgetContainer(rootTag);
};

// --- Appearance / color scheme --------------------------------------------------

export const Appearance = {
  getColorScheme: () => 'dark',
  setColorScheme() {},
  addChangeListener: () => ({ remove() {} }),
  removeChangeListener() {},
};
export function useColorScheme() {
  return 'dark';
}

// --- I18nManager ----------------------------------------------------------------

export const I18nManager = {
  isRTL: false,
  doLeftAndRightSwapInRTL: true,
  allowRTL() {},
  forceRTL(v) {
    I18nManager.isRTL = !!v;
  },
  swapLeftAndRightInRTL(v) {
    I18nManager.doLeftAndRightSwapInRTL = !!v;
  },
  getConstants: () => ({ isRTL: I18nManager.isRTL, doLeftAndRightSwapInRTL: I18nManager.doLeftAndRightSwapInRTL, localeIdentifier: 'en_US' }),
};

// --- small stubs ------------------------------------------------------------------

const subscription = { remove() {} };

export const Linking = {
  openURL: (url) => {
    if (typeof native.postMessage === 'function') native.postMessage('jsui:openURL', JSON.stringify({ url }));
    return Promise.resolve(true);
  },
  canOpenURL: () => Promise.resolve(false),
  getInitialURL: () => Promise.resolve(null),
  openSettings: () => Promise.resolve(),
  sendIntent: () => Promise.resolve(),
  addEventListener: () => subscription,
  removeEventListener() {},
};

export const Alert = {
  alert(title, message, buttons) {
    console.log('[Alert] ' + title + (message ? ': ' + message : ''));
    if (typeof native.postMessage === 'function') {
      native.postMessage('jsui:alert', JSON.stringify({ title, message, buttons: (buttons || []).map((b) => b && b.text) }));
    }
  },
  prompt(title, message) {
    console.log('[Alert.prompt] ' + title + (message ? ': ' + message : ''));
  },
};

export const Keyboard = {
  addListener: () => subscription,
  removeListener() {},
  removeAllListeners() {},
  dismiss() {},
  isVisible: () => false,
  metrics: () => undefined,
  scheduleLayoutAnimation() {},
};

export const LogBox = {
  ignoreLogs() {},
  ignoreAllLogs() {},
  install() {},
  uninstall() {},
};

export const AppState = {
  currentState: 'active',
  isAvailable: true,
  addEventListener: () => subscription,
  removeEventListener() {},
};

// BackHandler: Escape / gamepad B (when nothing else handled it). Modals first.
const backListeners = [];
let backInstalled = false;
function fireBack() {
  if (modalHandlesBack()) return true;
  for (let i = backListeners.length - 1; i >= 0; i--) if (backListeners[i]()) return true;
  return false;
}
export const BackHandler = {
  exitApp() {
    if (typeof native.postMessage === 'function') native.postMessage('jsui:exitApp', '{}');
  },
  addEventListener(type, handler) {
    if (type !== 'hardwareBackPress') return subscription;
    backListeners.push(handler);
    if (!backInstalled) {
      backInstalled = true;
      addGlobalListener('keydown', (e) => {
        if (!e.isDefaultPrevented() && (e.key === 'GoBack' || e.key === 'BrowserBack') && fireBack()) e.preventDefault();
      });
      addGlobalListener('gamepadbutton', (e) => {
        // Modals handle B themselves; only plain back handlers here.
        if (!e.isDefaultPrevented() && e.buttonIndex === 1 && e.pressed) {
          for (let i = backListeners.length - 1; i >= 0; i--) {
            if (backListeners[i]()) {
              e.preventDefault();
              break;
            }
          }
        }
      });
    }
    return {
      remove() {
        const i = backListeners.indexOf(handler);
        if (i >= 0) backListeners.splice(i, 1);
      },
    };
  },
  removeEventListener(_type, handler) {
    const i = backListeners.indexOf(handler);
    if (i >= 0) backListeners.splice(i, 1);
  },
};

export const AccessibilityInfo = {
  isScreenReaderEnabled: () => Promise.resolve(false),
  isReduceMotionEnabled: () => Promise.resolve(false),
  isBoldTextEnabled: () => Promise.resolve(false),
  isGrayscaleEnabled: () => Promise.resolve(false),
  isInvertColorsEnabled: () => Promise.resolve(false),
  isReduceTransparencyEnabled: () => Promise.resolve(false),
  prefersCrossFadeTransitions: () => Promise.resolve(false),
  addEventListener: () => subscription,
  announceForAccessibility() {},
  announceForAccessibilityWithOptions() {},
  setAccessibilityFocus() {},
  sendAccessibilityEvent() {},
  getRecommendedTimeoutMillis: (ms) => Promise.resolve(ms),
};

export const Vibration = {
  vibrate(pattern) {
    if (typeof native.postMessage === 'function') native.postMessage('jsui:vibrate', JSON.stringify({ pattern: pattern == null ? 400 : pattern }));
  },
  cancel() {},
};

export const Share = {
  share: () => Promise.resolve({ action: 'dismissedAction' }),
  sharedAction: 'sharedAction',
  dismissedAction: 'dismissedAction',
};

// The runtime's clipboard (the embedder's, or in-process) when available;
// TextInput copy / paste uses the same one.
let clip = '';
const readClip = () => (typeof native.getClipboard === 'function' ? String(native.getClipboard()) : clip);
export const Clipboard = {
  getString: () => Promise.resolve(readClip()),
  setString(s) {
    if (typeof native.setClipboard === 'function') native.setClipboard(String(s));
    else clip = String(s);
  },
  hasString: () => Promise.resolve(readClip().length > 0),
};

export const ToastAndroid = { SHORT: 0, LONG: 1, TOP: 0, BOTTOM: 1, CENTER: 2, show: (m) => console.log('[Toast] ' + m), showWithGravity: (m) => console.log('[Toast] ' + m), showWithGravityAndOffset: (m) => console.log('[Toast] ' + m) };
export const ActionSheetIOS = { showActionSheetWithOptions() {}, showShareActionSheetWithOptions() {}, dismissActionSheet() {} };
export const PermissionsAndroid = {
  PERMISSIONS: {},
  RESULTS: { GRANTED: 'granted', DENIED: 'denied', NEVER_ASK_AGAIN: 'never_ask_again' },
  check: () => Promise.resolve(false),
  request: () => Promise.resolve('denied'),
  requestMultiple: () => Promise.resolve({}),
};
export const Settings = { get: () => undefined, set() {}, watchKeys: () => 0, clearWatch() {} };
export const Systrace = { isEnabled: () => false, beginEvent() {}, endEvent() {}, beginAsyncEvent: () => 0, endAsyncEvent() {}, counterEvent() {} };
export const DevSettings = { addMenuItem() {}, reload: () => typeof native.reload === 'function' && native.reload() };
export const DevMenu = { show() {} };
export const DeviceInfo = { getConstants: () => ({ Dimensions: { window: dims(), screen: dims() } }) };
export const Networking = {};
export const PushNotificationIOS = {};
export const NativeDialogManagerAndroid = null;
export const UTFSequence = { BOM: '﻿', BULLET: '•', BULLET_SP: ' • ', MIDDOT: '·', MIDDOT_SP: ' · ', MIDDOT_KATAKANA: '・', MDASH: '—', MDASH_SP: ' — ', NDASH: '–', NDASH_SP: ' – ', NEWLINE: '\n', NBSP: ' ', PIZZA: '🍕', TRIANGLE_LEFT: '◀', TRIANGLE_RIGHT: '▶' };

// InteractionManager: no long-running interactions in JSUI; run soon.
export const InteractionManager = {
  Events: { interactionStart: 'interactionStart', interactionComplete: 'interactionComplete' },
  runAfterInteractions(task) {
    let cancelled = false;
    const promise = new Promise((resolve) => {
      setTimeout(() => {
        if (cancelled) return;
        let r;
        if (typeof task === 'function') r = task();
        else if (task && typeof task.run === 'function') r = task.run();
        resolve(r);
      }, 0);
    });
    return { then: promise.then.bind(promise), done: promise.then.bind(promise), cancel: () => (cancelled = true) };
  },
  createInteractionHandle: () => 0,
  clearInteractionHandle() {},
  addListener: () => subscription,
  setDeadline() {},
};

export { LayoutAnimation } from './layoutAnimation.js';

// --- event emitters ------------------------------------------------------------------

export class EventEmitter {
  constructor() {
    this._l = new Map();
  }
  addListener(type, listener, context) {
    let set = this._l.get(type);
    if (!set) this._l.set(type, (set = new Set()));
    const entry = { fn: listener, ctx: context };
    set.add(entry);
    return { remove: () => set.delete(entry) };
  }
  emit(type, ...args) {
    const set = this._l.get(type);
    if (set) for (const e of Array.from(set)) e.fn.apply(e.ctx, args);
  }
  removeAllListeners(type) {
    if (type === undefined) this._l.clear();
    else this._l.delete(type);
  }
  listenerCount(type) {
    const set = this._l.get(type);
    return set ? set.size : 0;
  }
}

export const DeviceEventEmitter = new EventEmitter();
export const NativeAppEventEmitter = DeviceEventEmitter;
export class NativeEventEmitter extends EventEmitter {}

// --- native module shims ---------------------------------------------------------------

export const NativeModules = new Proxy(
  {},
  {
    get(target, key) {
      if (typeof key !== 'string' || key in target) return target[key];
      warnOnce('nm:' + key, `[jsui] NativeModules.${key} is not available`);
      return undefined;
    },
  },
);
export const TurboModuleRegistry = {
  get: () => null,
  getEnforcing(name) {
    throw new Error(`TurboModuleRegistry.getEnforcing(...): '${name}' could not be found (not available in JSUI)`);
  },
};
// UIManager.measure*: null for unknown nodes; after the next layout when JS
// runs on a thread of its own (native.asyncMeasure).
function measureRaw(id, cb) {
  if (native.asyncMeasure && typeof native.measureAsync === 'function') native.measureAsync(id, cb);
  else cb(typeof native.measure === 'function' ? native.measure(id) : null);
}

export const UIManager = {
  getViewManagerConfig: () => null,
  hasViewManagerConfig: () => false,
  measure(id, cb) {
    measureRaw(id, (m) => {
      if (m && cb) cb(m.x, m.y, m.width, m.height, m.pageX, m.pageY);
    });
  },
  measureInWindow(id, cb) {
    measureRaw(id, (m) => {
      if (m && cb) cb(m.pageX, m.pageY, m.width, m.height);
    });
  },
  setLayoutAnimationEnabledExperimental() {},
  configureNextLayoutAnimation(config, onEnd, onFail) {
    LayoutAnimation.configureNext(config, onEnd, onFail);
  },
  dispatchViewManagerCommand() {},
  focus: (id) => typeof native.focus === 'function' && native.focus(id),
  blur: (id) => typeof native.blur === 'function' && native.blur(id),
};

export function findNodeHandle(ref) {
  if (ref == null) return null;
  if (typeof ref === 'number') return ref;
  if (typeof ref.id === 'number') return ref.id;
  if (ref.current) return findNodeHandle(ref.current);
  if (typeof ref.getNativeScrollRef === 'function') return findNodeHandle(ref.getNativeScrollRef());
  return null;
}

export function requireNativeComponent(name) {
  return function NativeComponent(props) {
    warnOnce('rnc:' + name, `[jsui] native component '${name}' is not available; rendering a View`);
    return h('View', props);
  };
}
export const codegenNativeComponent = requireNativeComponent;
export const codegenNativeCommands = () => new Proxy({}, { get: () => () => {} });
export function registerCallableModule() {}
export const PlatformColor = (...names) => names[names.length - 1];
export const DynamicColorIOS = (t) => (t && (t.dark || t.light)) || null;

// --- jsui-specific -------------------------------------------------------------------

export { onHostMessage } from './events.js';
import { onHostMessage } from './events.js';
import { call as hostCall, handle as hostHandle } from './hostData.js';
export { HostState, useHostState, useHostStateSelector, useHostHandler } from './hostData.js';
export { HostBuffer, useHostBuffer, useHostBufferSelector, useHostBufferListener } from './hostBuffer.js';
export { HostTextures, useHostTexture } from './hostTexture.js';

/**
 * Game <-> UI messages (postMessage / onMessage; payloads are JSON-serialized)
 * and calls: `await Host.call(name, args, {timeout, signal})` asks the
 * embedder; `Host.handle(name, fn)` answers jsui_call_ui from the embedder.
 * Shared state is HostState / useHostState.
 */
export const Host = {
  call: hostCall,
  handle: hostHandle,
  postMessage(name, data) {
    if (typeof native.postMessage !== 'function') {
      warnOnce('postMessage', '[jsui] __jsui_native.postMessage is not available');
      return;
    }
    native.postMessage(String(name), data === undefined ? '' : JSON.stringify(data));
  },
  onMessage: onHostMessage,
};

export function useHostMessage(name, callback) {
  const ref = useLatest(callback);
  useEffect(() => onHostMessage(name, (data, n) => ref.current && ref.current(data, n)), [name, ref]);
}

function useLatest(value) {
  const [box] = useState(() => ({ current: value }));
  box.current = value;
  return box;
}

// --- Fonts -----------------------------------------------------------------------
//
// expo-font compatible (loadAsync / useFonts / isLoaded / unloadAsync ...) plus
// JSUI extras. A face is (family, weight, italic, source); the same face loads
// once. weight 0 / italic -1 mean "read it from the file" (native contract).

const FONT_WEIGHTS = {
  thin: 100, hairline: 100, extralight: 200, ultralight: 200, light: 300,
  normal: 400, regular: 400, book: 400, medium: 500, semibold: 600, demibold: 600,
  bold: 700, extrabold: 800, ultrabold: 800, black: 900, heavy: 900,
};
const fontFaces = new Map(); // key -> { family, state: 'loading'|'loaded'|'unsupported', promise }
const bytesIds = new WeakMap();
let nextBytesId = 1;

function fontWeightArg(w) {
  if (w == null || w === '') return 0;
  if (typeof w === 'number') return w > 0 ? Math.min(1000, Math.round(w)) : 0;
  const s = String(w).trim().toLowerCase().replace(/[\s_-]/g, '');
  if (/^\d+$/.test(s)) return fontWeightArg(+s);
  if (s in FONT_WEIGHTS) return FONT_WEIGHTS[s];
  warnOnce('fontWeight:' + s, `[jsui] Fonts: unknown font weight '${w}', using the weight in the file`);
  return 0;
}

function fontItalicArg(o) {
  const st = o.style != null ? o.style : o.fontStyle;
  if (st === 'italic' || st === 'oblique') return 1;
  if (st === 'normal') return 0;
  if (o.italic != null) return o.italic ? 1 : 0;
  return -1;
}

const isFontBytes = (v) => v instanceof ArrayBuffer || ArrayBuffer.isView(v);

/** source -> { url } | { bytes } | { assetId } | null */
function fontSource(src) {
  if (src == null) return null;
  if (typeof src === 'string') return src ? { url: src } : null;
  if (typeof src === 'number') return { assetId: src };
  if (isFontBytes(src)) return { bytes: src };
  if (typeof src === 'object') {
    if (src.uri != null) return fontSource(src.uri);
    if (src.source != null) return fontSource(src.source);
    if ('default' in src) return fontSource(src.default);
  }
  return null;
}

function fontFace(family, source, options) {
  const o = options || {};
  const s = fontSource(source);
  const weight = fontWeightArg(o.weight != null ? o.weight : o.fontWeight);
  const italic = fontItalicArg(o);
  let id = '';
  if (s && s.url) id = 'u:' + s.url;
  else if (s && s.bytes) {
    id = bytesIds.get(s.bytes);
    if (!id) bytesIds.set(s.bytes, (id = 'b:' + nextBytesId++));
  }
  const fam = family == null ? '' : String(family);
  return { family: fam, s, weight, italic, key: fam + '|' + weight + '|' + italic + '|' + id };
}

/** A map value -> face descriptors: a source, { uri|source, weight, style }, or an array of those. */
function faceList(value) {
  if (Array.isArray(value)) return value.flatMap(faceList);
  if (value && typeof value === 'object' && !isFontBytes(value)) {
    if (value.uri == null && value.source == null && 'default' in value) return faceList(value.default);
    return [{ source: value, options: value }];
  }
  return [{ source: value, options: null }];
}

function familyNames(nameOrMap) {
  if (nameOrMap == null) return [];
  if (typeof nameOrMap === 'string') return [nameOrMap];
  if (Array.isArray(nameOrMap)) return nameOrMap.map(String);
  return Object.keys(nameOrMap);
}

function mapFaces(nameOrMap, source) {
  if (typeof nameOrMap === 'string') return faceList(source).map((f) => [nameOrMap, f]);
  const map = nameOrMap || {};
  return Object.keys(map).flatMap((k) => faceList(map[k]).map((f) => [k, f]));
}

function fontError(e) {
  return e instanceof Error ? e : new Error(String(e && e.message ? e.message : e));
}

function hasFaces(family) {
  for (const f of fontFaces.values()) if (f.family === family) return true;
  return false;
}

function nativeUnload(family) {
  if (typeof native.unloadFont !== 'function') {
    warnOnce('unloadFont', '[jsui] __jsui_native.unloadFont is not available');
    return false;
  }
  return !!native.unloadFont(family);
}

function loadFace(face) {
  const { family, s, weight, italic, key } = face;
  const label = `[jsui] Fonts.load('${family}')`;
  if (!s) return Promise.reject(new Error(`${label}: no font source (expected a URL, require('./font.ttf'), { uri } or bytes)`));
  if (s.assetId != null) {
    warnOnce('fontAssetId', `${label}: numeric asset ids are not supported; bundle the font with the jsui CLI (require('./font.ttf') is a URL) or pass { uri }`);
    return Promise.reject(new Error(`${label}: unsupported asset id ${s.assetId}`));
  }
  const prev = fontFaces.get(key);
  if (prev) return prev.promise;
  const f = { family, state: 'loading', promise: null };
  fontFaces.set(key, f);
  const settle = (ok) => {
    if (fontFaces.get(key) !== f) {
      // Unloaded while loading: undo the late registration.
      if (ok && !hasFaces(family) && typeof native.unloadFont === 'function') native.unloadFont(family);
      return !!ok;
    }
    f.state = ok ? 'loaded' : 'unsupported';
    return !!ok;
  };
  const fail = (e) => {
    if (fontFaces.get(key) === f) fontFaces.delete(key);
    throw fontError(e);
  };
  if (s.bytes) {
    if (typeof native.loadFontData !== 'function') {
      warnOnce('loadFontData', '[jsui] __jsui_native.loadFontData is not available; font bytes are ignored');
      f.promise = Promise.resolve(settle(false));
    } else {
      try {
        // Synchronous: isLoaded() is true right after the call.
        f.promise = Promise.resolve(settle(native.loadFontData(family, s.bytes, weight, italic) !== false));
      } catch (e) {
        f.promise = Promise.resolve().then(() => fail(e));
      }
    }
  } else if (typeof native.loadFont !== 'function') {
    warnOnce('loadFont', '[jsui] __jsui_native.loadFont is not available');
    f.promise = Promise.resolve(settle(false));
  } else {
    let p;
    try {
      p = Promise.resolve(native.loadFont(family, String(s.url), weight, italic));
    } catch (e) {
      p = Promise.reject(e);
    }
    f.promise = p.then((ok) => settle(ok !== false), fail);
  }
  return f.promise;
}

export const Fonts = {
  /**
   * Registers one face: Fonts.load(family, source, { weight, style }).
   * source: URL, require('./x.ttf'), { uri }, { default }, ArrayBuffer / typed array.
   * weight / style omitted: read from the file. Resolves true when usable
   * (false when the runtime has no font support).
   */
  load(family, source, options) {
    return loadFace(fontFace(family, source, options));
  },
  /**
   * expo-font: loadAsync('Family', source) or loadAsync({ Family: source | face | [faces] }),
   * face = { uri | source, weight, style }.
   */
  loadAsync(nameOrMap, source) {
    return Promise.all(mapFaces(nameOrMap, source).map(([k, f]) => Fonts.load(k, f.source, f.options))).then(() => undefined);
  },
  /** True once a face of the family loaded through Fonts (or registered natively). */
  isLoaded(family) {
    for (const f of fontFaces.values()) if (f.family === family && f.state === 'loaded') return true;
    if (typeof native.getFontFamilies === 'function') return native.getFontFamilies().indexOf(family) >= 0;
    return false;
  },
  isLoading(family) {
    for (const f of fontFaces.values()) if (f.family === family && f.state === 'loading') return true;
    return false;
  },
  /** Registered families (native list incl. built-in Inter when available). */
  getLoadedFonts() {
    if (typeof native.getFontFamilies === 'function') return Array.from(native.getFontFamilies());
    const out = [];
    for (const f of fontFaces.values()) if (f.state === 'loaded' && out.indexOf(f.family) < 0) out.push(f.family);
    return out;
  },
  /** Unloads families loaded through Fonts: unloadAsync('Family' | ['A','B'] | { Family: ... }). */
  unloadAsync(nameOrMap) {
    for (const family of familyNames(nameOrMap)) {
      if (!hasFaces(family)) continue;
      for (const [k, f] of fontFaces) if (f.family === family) fontFaces.delete(k);
      nativeUnload(family);
    }
    return Promise.resolve();
  },
  /** Unloads every family loaded through Fonts (built-in and host fonts stay). */
  unloadAllAsync() {
    const families = [];
    for (const f of fontFaces.values()) if (families.indexOf(f.family) < 0) families.push(f.family);
    return Fonts.unloadAsync(families);
  },
  /** Families tried, in order, for glyphs the styled font lacks (emoji, CJK). [] clears. */
  setFallbacks(families) {
    const list = families == null ? [] : Array.isArray(families) ? families : String(families).split(',');
    const clean = list.map((f) => String(f).trim().replace(/^["']|["']$/g, '')).filter(Boolean);
    if (typeof native.setFontFallbacks !== 'function') {
      warnOnce('setFontFallbacks', '[jsui] __jsui_native.setFontFallbacks is not available');
      return;
    }
    native.setFontFallbacks(clean);
  },
  /** Family used when a style names none (default 'Inter'). */
  setDefaultFamily(family) {
    if (typeof native.setDefaultFontFamily !== 'function') {
      warnOnce('setDefaultFontFamily', '[jsui] __jsui_native.setDefaultFontFamily is not available');
      return;
    }
    native.setDefaultFontFamily(family == null ? '' : String(family));
  },
};

function fontMapState(map) {
  const faces = mapFaces(map || {}).map(([k, f]) => fontFace(k, f.source, f.options));
  let ready = true;
  for (const f of faces) {
    const e = fontFaces.get(f.key);
    if (!e || e.state === 'loading') ready = false;
  }
  return { key: faces.map((f) => f.key).join('\n'), ready };
}

/**
 * useFonts({ Family: source | face | [faces] }) -> [loaded, error] (expo-font
 * compatible). Fonts that are already loaded give [true, null] on the first render.
 */
export function useFonts(map) {
  const { key, ready } = fontMapState(map);
  const [st, setSt] = useState(null);
  const cur = st && st.key === key ? st : { key, loaded: ready, error: null };
  useEffect(() => {
    if (ready) {
      if (!cur.loaded) setSt({ key, loaded: true, error: null });
      return undefined;
    }
    let alive = true;
    Fonts.loadAsync(map || {}).then(
      () => alive && setSt({ key, loaded: true, error: null }),
      (e) => alive && setSt({ key, loaded: false, error: fontError(e) }),
    );
    return () => {
      alive = false;
    };
  }, [key]);
  return [cur.loaded, cur.error];
}

// Gamepad state, updated from native gamepad events.
const pads = new Map();
const padListeners = new Set();
let padVersion = 0;
function padState(index) {
  let p = pads.get(index);
  if (!p) {
    p = { index, id: '', connected: false, buttons: [], axes: [] };
    pads.set(index, p);
  }
  return p;
}
function padChanged() {
  padVersion++;
  for (const l of Array.from(padListeners)) l();
}
addGlobalListener('gamepadconnected', (e) => {
  const ne = e.nativeEvent;
  const p = padState(ne.gamepad || 0);
  pads.set(p.index, Object.assign({}, p, { connected: ne.connected !== false, id: ne.id || p.id }));
  padChanged();
});
addGlobalListener('gamepadbutton', (e) => {
  const p = padState(e.gamepad || 0);
  const buttons = p.buttons.slice();
  buttons[e.buttonIndex] = e.value != null ? +e.value : e.pressed ? 1 : 0;
  pads.set(p.index, Object.assign({}, p, { connected: true, buttons }));
  padChanged();
});
addGlobalListener('gamepadaxis', (e) => {
  const p = padState(e.gamepad || 0);
  const axes = p.axes.slice();
  axes[e.axis] = +e.value || 0;
  pads.set(p.index, Object.assign({}, p, { connected: true, axes }));
  padChanged();
});

/**
 * useGamepad({ index = 0, onButton, onAxis }) -> { connected, id, buttons, axes }
 * onButton(e) / onAxis(e) receive the synthetic event (buttonIndex, value,
 * pressed / axis, value); call e.preventDefault() to stop the default focus
 * navigation / activation for that event.
 */
export function useGamepad(options) {
  const index = (options && options.index) || 0;
  const cbs = useLatest(options || {});
  const state = useSyncExternalStore(
    (cb) => {
      padListeners.add(cb);
      return () => padListeners.delete(cb);
    },
    () => padState(index),
  );
  useEffect(() => {
    const offB = addGlobalListener('gamepadbutton', (e) => {
      if ((e.gamepad || 0) === index && cbs.current.onButton) cbs.current.onButton(e);
    });
    const offA = addGlobalListener('gamepadaxis', (e) => {
      if ((e.gamepad || 0) === index && cbs.current.onAxis) cbs.current.onAxis(e);
    });
    return () => {
      offB();
      offA();
    };
  }, [index, cbs]);
  return state;
}
export function getGamepadVersion() {
  return padVersion;
}

/**
 * useFocus() -> { ref, focused, focus(), blur(), focusProps }
 * Spread focusProps on any View to make it focusable and track its focus.
 */
export function useFocus(options) {
  const [focused, setFocused] = useState(false);
  const [api] = useState(() => {
    const ref = { current: null };
    return {
      ref,
      focus: () => ref.current && ref.current.focus(),
      blur: () => ref.current && ref.current.blur(),
    };
  });
  const opts = useLatest(options || {});
  const [focusProps] = useState(() => ({
    ref: api.ref,
    focusable: true,
    onFocus: (e) => {
      setFocused(true);
      if (opts.current.onFocus) opts.current.onFocus(e);
    },
    onBlur: (e) => {
      setFocused(false);
      if (opts.current.onBlur) opts.current.onBlur(e);
    },
  }));
  useEffect(() => {
    if (options && options.autoFocus) api.focus();
  }, []);
  return { ref: api.ref, focused, focus: api.focus, blur: api.blur, focusProps };
}

/** Focus / blur by ref or node id. */
export function focus(target) {
  const id = findNodeHandle(target);
  if (id != null && typeof native.focus === 'function') native.focus(id);
}
export function blur(target) {
  const id = findNodeHandle(target);
  if (id != null && typeof native.blur === 'function') native.blur(id);
}
