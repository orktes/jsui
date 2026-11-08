// The window manager's side of the shell: shared state the host publishes and
// the messages that ask it for changes (see ../README.md for the protocol).
import { useEffect, useMemo, useReducer } from 'react';
import { Host, HostState, useHostState } from 'react-native';

export const send = (name, payload = {}) => Host.postMessage(name, payload);

export const wm = {
  focus: (id) => send('wm:focus', { id }),
  move: (id, x, y) => send('wm:move', { id, x, y }),
  resize: (id, x, y, w, h) => send('wm:resize', { id, x, y, w, h }),
  close: (id) => send('wm:close', { id }),
  closeAll: () => send('wm:closeAll'),
  minimize: (id) => send('wm:minimize', { id }),
  restore: (id) => send('wm:restore', { id }),
  maximize: (id, on) => send('wm:maximize', { id, on }),
  snap: (id, side) => send('wm:snap', { id, side }),
  split: (left, right, ratio) => send('wm:split', ratio == null ? { left, right } : { left, right, ratio }),
  splitRatio: (ratio) => send('wm:splitRatio', { ratio }),
  fullscreen: (id, on) => send('wm:fullscreen', { id, on }),
  release: (id) => send('wm:release', { id }),
  // The on-screen keyboard: its height on screen (0: hidden), and that the
  // user closed one an app had asked for.
  osk: (height) => send('wm:osk', { height }),
  textInputClosed: () => send('wm:textInputClosed'),
  // A blurred copy of the screen as host texture "wm:backdrop" (on: false drops it).
  snapshot: (on) => send('wm:snapshot', { on }),
  // A short buzz (hosts with a vibrator: hfd on Ubuntu Touch).
  haptic: (ms = 12) => send('wm:haptic', { ms }),
  // A notification (wm:notification's id) was tapped: its default action.
  notificationAction: (id) => send('wm:notificationAction', { id }),
  // The screen's backlight and the output volume, 0..1 (hosts that publish
  // system.backlight / system.volume).
  brightness: (value) => send('wm:brightness', { value }),
  volume: (value, muted = false) => send('wm:volume', { value, muted }),
  // Wi-Fi over the host's NetworkManager (hosts whose system.wifi is not
  // null): wm:wifiNetworks answers a scan, wm:wifiResult a join.
  wifiScan: (rescan = false) => send('wm:wifiScan', { rescan }),
  wifiConnect: (ssid, password = '') => send('wm:wifiConnect', { ssid, password }),
  wifiRadio: (on) => send('wm:wifiRadio', { on }),
  launch: (exec) => send('wm:launch', { exec }),
  keyboard: (grab) => send('wm:keyboard', { grab }),
  typeKey: (key, mods = 0) => send('wm:typeKey', { key, mods }),
  shellRegions: (rects) => send('wm:shellRegions', { rects }),
  anchors: (map) => send('wm:anchors', map),
  quit: () => send('wm:quit'),
};

const SCREEN = { width: 1280, height: 800, pixelRatio: 1.5, topBar: 40, dock: 84, homeBar: 24, titleBar: 46, border: 12, touch: true };

export function useScreen() {
  return useHostState('wm.screen', SCREEN);
}

export function useApps() {
  return useHostState('wm.apps', []);
}

export function useSystem() {
  return useHostState('wm.system', { user: 'user', hostname: 'linux', battery: null });
}

// Windows in stacking order (bottom to top), plus the active id.
// Windows swiped away in the switcher: gone from the shell at once (an app
// can take a second to exit, and its window showed again meanwhile), and
// released without a closing animation when the host unmaps them.
const dismissed = new Set();
const dismissListeners = new Set();
export function dismissWindow(id) {
  dismissed.add(id);
  dismissListeners.forEach((l) => l());
  wm.close(id);
}
export const wasDismissed = (id) => dismissed.has(id);

export function useWindows() {
  const hostMap = useHostState('wm.windows', {});
  const stack = useHostState('wm.stack', []);
  const active = useHostState('wm.active', 0);
  const [gen, bump] = useReducer((n) => n + 1, 0);
  useEffect(() => {
    dismissListeners.add(bump);
    return () => dismissListeners.delete(bump);
  }, []);
  useEffect(() => {
    for (const id of dismissed) {
      if (hostMap[id]) continue;
      dismissed.delete(id);
      send('wm:release', { id });
    }
  }, [hostMap]);
  const map = useMemo(() => {
    if (!dismissed.size) return hostMap;
    const m = { ...hostMap };
    for (const id of dismissed) delete m[id];
    return m;
  }, [hostMap, gen]); // eslint-disable-line react-hooks/exhaustive-deps
  const list = useMemo(() => {
    const out = [];
    for (const id of stack) if (map[id]) out.push(map[id]);
    for (const id in map) if (!stack.includes(map[id].id)) out.push(map[id]);
    return out;
  }, [map, stack]);
  return { map, list, active: dismissed.has(active) ? 0 : active };
}

export function usePopups() {
  return useHostState('wm.popups', {});
}

export const DEFAULT_SETTINGS = {
  darkMode: true,
  tabletMode: true, // apps open full screen; split view and the switcher for multitasking
  brightness: 1,
  volume: 0.6,
  muted: false,
  wifi: true,
  bluetooth: false,
  doNotDisturb: false,
  nightLight: false,
  autoRotate: false, // follow the accelerometer (hosts with system.rotate)
  osk: false,
  homeApps: null, // the apps picked for home (ids); null: homeApps() picks
  pinned: ['org.codeberg.dnkl.foot', 'debian-xterm', 'xterm', 'galculator', 'org.xfce.mousepad', 'pcmanfm', 'xclock', 'xeyes'],
};

export function useSettings() {
  const s = useHostState('wm.settings', DEFAULT_SETTINGS);
  // One object per change of the state: a new one per render made every
  // memoized panel that takes `settings` (the launcher, ~10 ms of its grid on
  // the Tab A7) render again with each render of the shell.
  return useMemo(() => ({ ...DEFAULT_SETTINGS, ...s }), [s]);
}

export function setSetting(key, value) {
  const cur = { ...DEFAULT_SETTINGS, ...(HostState.get('wm.settings') || {}) };
  HostState.set('wm.settings', { ...cur, [key]: value });
}

// Which app (from wm.apps) a window belongs to; windows of apps without a
// desktop file group by their WM_CLASS.
export function appKeyOf(win) {
  return win.appId || (win.wmClass ? 'class:' + win.wmClass.toLowerCase() : 'win:' + win.id);
}

// "ImageMagick (color depth=q16)" -> "ImageMagick".
export const cleanName = (name) => String(name || '').replace(/\s*\([^)]*\)\s*/g, ' ').trim();

// The app's name for a window: its desktop file's name, else its WM_CLASS
// ("galculator" -> "Galculator").
export function appName(win, apps) {
  const app = win.appId && apps.find((a) => a.id === win.appId);
  if (app) return cleanName(app.name);
  const c = win.wmClass || '';
  return c ? c.charAt(0).toUpperCase() + c.slice(1) : win.title || 'Window';
}

export function appTitle(win, apps) {
  return appName(win, apps);
}

// A window's title for its title bar: its own, unless that is only an id
// (Ubuntu Touch's Qt apps title their window with the app id,
// "clock.ubports"), when the app's name reads better.
export function windowTitle(win, apps) {
  const t = win.title || '';
  if (!t || (/^[\w.-]+$/.test(t) && t.includes('.')) || t === win.appId || t === win.wmClass) return appName(win, apps);
  return t;
}

// The apps on home: the picked ones (settings.homeApps), or until anything is
// picked a few everyday ones of those installed.
const HOME_DEFAULTS = [/morph-browser/, /^camera\.ubports_camera/, /gallery/, /system-settings/, /terminal/, /^clock\./];
export function homeApps(settings, apps) {
  if (Array.isArray(settings.homeApps)) return settings.homeApps.map((id) => apps.find((a) => a.id === id)).filter(Boolean);
  const out = [];
  for (const re of HOME_DEFAULTS) {
    const a = apps.find((x) => re.test(x.id) && !out.includes(x));
    if (a) out.push(a);
  }
  return out.length ? out : apps.slice(0, 6);
}

export function pinHome(settings, apps, id, on) {
  const ids = homeApps(settings, apps).map((a) => a.id).filter((x) => x !== id);
  setSetting('homeApps', on ? [...ids, id] : ids);
}
