// The window manager's side of the shell: shared state the host publishes and
// the messages that ask it for changes (see ../README.md for the protocol).
import { useMemo } from 'react';
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
  launch: (exec) => send('wm:launch', { exec }),
  keyboard: (grab) => send('wm:keyboard', { grab }),
  typeKey: (key, mods = 0) => send('wm:typeKey', { key, mods }),
  shellRegions: (rects) => send('wm:shellRegions', { rects }),
  anchors: (map) => send('wm:anchors', map),
  quit: () => send('wm:quit'),
};

const SCREEN = { width: 1280, height: 800, pixelRatio: 1.5, topBar: 40, dock: 84, titleBar: 46, border: 12, touch: true };

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
export function useWindows() {
  const map = useHostState('wm.windows', {});
  const stack = useHostState('wm.stack', []);
  const active = useHostState('wm.active', 0);
  const list = useMemo(() => {
    const out = [];
    for (const id of stack) if (map[id]) out.push(map[id]);
    for (const id in map) if (!stack.includes(map[id].id)) out.push(map[id]);
    return out;
  }, [map, stack]);
  return { map, list, active };
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
  osk: false,
  pinned: ['debian-xterm', 'xterm', 'galculator', 'org.xfce.mousepad', 'pcmanfm', 'xclock', 'xeyes'],
};

export function useSettings() {
  const s = useHostState('wm.settings', DEFAULT_SETTINGS);
  return { ...DEFAULT_SETTINGS, ...s };
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

export function appTitle(win, apps) {
  const app = win.appId && apps.find((a) => a.id === win.appId);
  return (app && app.name) || win.wmClass || win.title || 'Window';
}
