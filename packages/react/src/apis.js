// Non-component React Native APIs.

import { createElement as h, useEffect, useState, useSyncExternalStore, createContext } from 'react';
import { native, readViewport, warnOnce } from './native.js';
import { LayoutAnimation } from './layoutAnimation.js';
import { addGlobalListener } from './events.js';
import { render as renderRoot } from './renderer.js';
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

function onResize(ev) {
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

export function useWindowDimensions() {
  return useSyncExternalStore(subscribeDims, dims, dims);
}

export const PixelRatio = {
  get: () => viewport.scale,
  getFontScale: () => 1,
  getPixelSizeForLayoutSize: (size) => Math.round(size * viewport.scale),
  roundToNearestPixel: (size) => Math.round(size * viewport.scale) / viewport.scale,
  startDetecting() {},
};

// --- AppRegistry -------------------------------------------------------------------

const registry = new Map();
let started = false;
let autoStartScheduled = false;

const appStyle = { flex: 1, pointerEvents: 'box-none' };
export const RootTagContext = createContext(1);

export const AppRegistry = {
  registerComponent(appKey, getComponent) {
    registry.set(appKey, { getComponent });
    // RN native starts the registered app automatically. Do it at the end of
    // bundle evaluation (microtask), so a later explicit runApplication call
    // (react-native-web style) or other registrations still win.
    if (!autoStartScheduled) {
      autoStartScheduled = true;
      Promise.resolve().then(() => {
        if (!started && registry.has(appKey)) AppRegistry.runApplication(appKey, {});
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
  getApplication(appKey) {
    return { element: AppRegistry._element(appKey, {}) };
  },
  _element(appKey, params) {
    const entry = registry.get(appKey);
    if (!entry || !entry.getComponent) throw new Error(`AppRegistry: no component registered as "${appKey}"`);
    const App = entry.getComponent();
    return h(RootTagContext.Provider, { value: 1 }, h('View', { style: appStyle }, h(App, (params && params.initialProps) || {})));
  },
  runApplication(appKey, params) {
    const entry = registry.get(appKey);
    if (!entry) throw new Error(`AppRegistry: application "${appKey}" has not been registered`);
    started = true;
    if (entry.run) return entry.run(params || {});
    renderRoot(AppRegistry._element(appKey, params));
    return undefined;
  },
  unmountApplicationComponentAtRootTag() {
    renderRoot(null);
  },
  setWrapperComponentProvider() {},
  setComponentProviderInstrumentationHook() {},
  enableArchitectureIndicator() {},
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
export const UIManager = {
  getViewManagerConfig: () => null,
  hasViewManagerConfig: () => false,
  measure(id, cb) {
    const m = typeof native.measure === 'function' ? native.measure(id) : null;
    if (m && cb) cb(m.x, m.y, m.width, m.height, m.pageX, m.pageY);
  },
  measureInWindow(id, cb) {
    const m = typeof native.measure === 'function' ? native.measure(id) : null;
    if (m && cb) cb(m.pageX, m.pageY, m.width, m.height);
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

/** Game <-> UI messages. Payloads are JSON-serialized. */
export const Host = {
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

const loadedFonts = new Map();
export const Fonts = {
  /** Registers a font face from a URL (relative to the bundle). Resolves when usable. */
  load(family, url, options) {
    const weight = options && options.weight != null ? +String(options.weight).replace('normal', '400').replace('bold', '700') || 400 : 400;
    const italic = !!(options && options.style === 'italic');
    const key = family + '|' + weight + '|' + italic + '|' + url;
    let p = loadedFonts.get(key);
    if (p) return p;
    if (typeof native.loadFont !== 'function') {
      warnOnce('loadFont', '[jsui] __jsui_native.loadFont is not available');
      p = Promise.resolve(false);
    } else {
      p = Promise.resolve(native.loadFont(family, String(url), weight, italic));
    }
    loadedFonts.set(key, p);
    return p;
  },
  /** expo-font style: loadAsync({Family: url}) */
  loadAsync(map) {
    return Promise.all(Object.keys(map).map((k) => Fonts.load(k, map[k])));
  },
};

/**
 * useFonts({Family: url}) -> [loaded, error] (expo-font compatible).
 */
export function useFonts(map) {
  const [state, setState] = useState([false, null]);
  const key = Object.keys(map || {}).map((k) => k + '=' + map[k]).join('|');
  useEffect(() => {
    let alive = true;
    Fonts.loadAsync(map || {}).then(
      () => alive && setState([true, null]),
      (e) => alive && setState([false, e]),
    );
    return () => {
      alive = false;
    };
  }, [key]);
  return state;
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
