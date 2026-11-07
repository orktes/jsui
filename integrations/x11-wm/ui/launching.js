// Apps being started: each launch from the shell shows a loading window (the
// app's icon and a spinner, LaunchSplash in shell.jsx) until the app's first
// window is up, the host says the launch ended (wm:launchEnded / Failed), or
// LAUNCH_TIMEOUT_MS passes.
import { useEffect, useState } from 'react';
import { noteLaunch } from './anchors.js';
import { wm } from './wm.js';

export const LAUNCH_TIMEOUT_MS = 30000;

let launches = []; // { key, app, at, anchor }
let seq = 1;
const listeners = new Set();
const emit = () => listeners.forEach((l) => l(launches));

// Starts `app` from the icon at anchor `anchorName` (for the window to grow
// out of). `running`: the app has a window already (the host just brings it
// to the front), so no loading window.
export function startLaunch(app, anchorName, running = false) {
  if (anchorName) noteLaunch(app.id, anchorName);
  wm.launch(app.exec);
  if (running || launches.some((l) => l.app.id === app.id)) return;
  launches = [...launches, { key: seq++, app, at: Date.now() }];
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
