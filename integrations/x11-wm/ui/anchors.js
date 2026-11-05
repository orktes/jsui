// Where shell elements are on screen, for test drivers: the host writes them
// to --anchors <file> (screen px) so an xdotool script can tap "the launcher
// button" instead of a hard-coded point.
import { useCallback, useEffect, useRef } from 'react';
import { wm } from './wm.js';

const anchors = {};
let timer = null;

function flush() {
  timer = null;
  wm.anchors({ ...anchors });
}

export function setAnchor(name, rect) {
  if (rect) anchors[name] = rect;
  else delete anchors[name];
  if (!timer) timer = setTimeout(flush, 50);
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
