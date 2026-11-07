// Where shell elements are on screen, for test drivers: the host writes them
// to --anchors <file> (screen px) so an xdotool script can tap "the launcher
// button" instead of a hard-coded point.
import { useCallback, useEffect, useRef } from 'react';
import { wm } from './wm.js';

const anchors = {};
const gone = {};  // removed anchors' last rects (anchorRect)
let timer = null;

function flush() {
  timer = null;
  wm.anchors({ ...anchors });
}

export function setAnchor(name, rect) {
  if (rect) {
    anchors[name] = rect;
    delete gone[name];
  } else if (anchors[name]) {
    gone[name] = anchors[name];
    delete anchors[name];
  }
  if (!timer) timer = setTimeout(flush, 50);
}

// The last measured rect of an anchor ([x, y, w, h] in shell px), also
// just after it went away (a dock icon unmounts in the same commit as its
// app's last window, which then shrinks into it), or undefined.
export function anchorRect(name) {
  return anchors[name] || gone[name];
}

// Where an app was launched from (an icon's rect), for its first window to
// grow out of (Window.jsx). Taken once, within 30 s of the launch.
const launchOrigins = new Map();
export function noteLaunch(appId, anchorName) {
  const rect = anchors[anchorName];
  if (appId && rect) launchOrigins.set(appId, { rect, at: Date.now() });
}
export function takeLaunchOrigin(appId) {
  const o = appId && launchOrigins.get(appId);
  if (!o) return null;
  launchOrigins.delete(appId);
  return Date.now() - o.at < 30000 ? o.rect : null;
}

const measurers = new Set();

// Measures every anchor again (after a ScrollView scrolled: no onLayout then).
export function remeasureAnchors() {
  setTimeout(() => measurers.forEach((m) => m()), 30);
}

// <View ref={ref} onLayout={onLayout}>: records the view's window rect.
export function useAnchor(name) {
  const ref = useRef(null);
  const onLayout = useCallback(() => {
    const v = ref.current;
    if (v && v.measureInWindow) v.measureInWindow((x, y, w, h) => setAnchor(name, [x, y, w, h]));
  }, [name]);
  useEffect(() => {
    measurers.add(onLayout);
    return () => {
      measurers.delete(onLayout);
      setAnchor(name, null);
    };
  }, [name, onLayout]);
  return { ref, onLayout };
}
