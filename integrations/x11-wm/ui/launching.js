// Apps being started: each launch from the shell shows a loading window (the
// app's icon and a spinner, LaunchSplash in shell.jsx) until the app's first
// window is up, the host says the launch ended (wm:launchEnded / Failed), or
// LAUNCH_TIMEOUT_MS passes. Several can run at once: only the newest shows
// its card; the others (`bg`) wait as cards in the switcher, as does every
// one while the switcher is up (backgroundLaunches).
import { useEffect, useState } from 'react';
import { noteLaunch } from './anchors.js';
import { wm } from './wm.js';

export const LAUNCH_TIMEOUT_MS = 30000;

let launches = []; // { key, app, at, bg }
let seq = 1;
const listeners = new Set();
const emit = () => listeners.forEach((l) => l(launches));

// Starts `app` from the icon at anchor `anchorName` (for the window to grow
// out of). `running`: the app has a window already (the host just brings it
// to the front), so no loading window.
export function startLaunch(app, anchorName, running = false) {
  if (anchorName) noteLaunch(app.id, anchorName);
  wm.launch(app.exec);
  if (running) return;
  const again = launches.find((l) => l.app.id === app.id);
  launches = launches.map((l) => (l === again || l.bg ? l : { ...l, bg: true }));
  if (again) launches = launches.map((l) => (l === again ? { ...l, bg: false } : l));
  else launches = [...launches, { key: seq++, app, at: Date.now() }];
  emit();
}

// Every launch to the switcher (it is opening, or home was asked for).
export function backgroundLaunches() {
  if (!launches.some((l) => !l.bg)) return;
  launches = launches.map((l) => (l.bg ? l : { ...l, bg: true }));
  emit();
}

// The launch `key` back in front (its card picked in the switcher).
export function foregroundLaunch(key) {
  launches = launches.map((l) => ({ ...l, bg: l.key !== key }));
  emit();
}

export function endLaunch(match) {
  const next = launches.filter((l) => !match(l));
  if (next.length !== launches.length) {
    launches = next;
    emit();
  }
}

export function useLaunching() {
  const [list, setList] = useState(launches);
  useEffect(() => {
    listeners.add(setList);
    setList(launches);
    return () => listeners.delete(setList);
  }, []);
  return list;
}
