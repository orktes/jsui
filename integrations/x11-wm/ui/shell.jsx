// JSUI desktop shell for jsui-wm (../main.cpp), made for 11" touchscreens.
//
// Everything on screen is drawn here: the wallpaper, every client window
// (its pixels are host textures, its frame is ours), the top bar, the dock,
// the launcher, the overview, the panels, the on-screen keyboard and the lock
// screen. The host publishes the window list as shared state and does the X
// side when we ask (wm.js). Whatever the shell puts above the windows also has
// to take the input there, so the shell tells the host which rectangles it
// covers (wm:shellRegions) whenever that changes.
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, AppRegistry, Easing, DevSettings, PanResponder, Pressable, StyleSheet, Text, View, useHostMessage, useWindowDimensions, HostTexture } from 'react-native';
import Svg, { Defs, Ellipse, RadialGradient, Stop } from 'react-native-svg';
import { Dock, DockMenu, HomeBar, TopBar, dateText, timeText, useClock } from './bars.jsx';
import { Icon } from './icons.jsx';
import { takeLaunchOrigin, useAnchor } from './anchors.js';
import { AppIcon } from './AppIcon.jsx';
import { LAUNCH_TIMEOUT_MS, endLaunch, startLaunch, useLaunching } from './launching.js';
import { useNativeDragX } from './drag.js';
import { Keyboard } from './keyboard.jsx';
import { CalendarPanel, Launcher, LockScreen, Overview, QuickSettings, Toasts } from './panels.jsx';
import { ThemeContext, dark, font, light } from './theme.js';
import { Window } from './Window.jsx';
import { appName, cleanName, setSetting, useApps, usePopups, useScreen, useSettings, useSystem, useWindows, wm } from './wm.js';

// ---------------------------------------------------------------- Wallpaper

function Wallpaper({ width, height, t }) {
  return (
    <View pointerEvents="none" style={[StyleSheet.absoluteFill, { experimental_backgroundImage: `linear-gradient(160deg, ${t.wallTop} 0%, ${t.wallMid} 55%, ${t.wallBottom} 100%)` }]}>
      <Svg width={width} height={height} style={StyleSheet.absoluteFill}>
        <Defs>
          <RadialGradient id="ga" cx="50%" cy="50%" r="50%">
            <Stop offset="0" stopColor={t.glowA} />
            <Stop offset="1" stopColor={t.glowA} stopOpacity="0" />
          </RadialGradient>
          <RadialGradient id="gb" cx="50%" cy="50%" r="50%">
            <Stop offset="0" stopColor={t.glowB} />
            <Stop offset="1" stopColor={t.glowB} stopOpacity="0" />
          </RadialGradient>
          <RadialGradient id="gc" cx="50%" cy="50%" r="50%">
            <Stop offset="0" stopColor={t.glowC} />
            <Stop offset="1" stopColor={t.glowC} stopOpacity="0" />
          </RadialGradient>
        </Defs>
        <Ellipse cx={width * 0.18} cy={height * 0.22} rx={width * 0.42} ry={height * 0.5} fill="url(#ga)" />
        <Ellipse cx={width * 0.86} cy={height * 0.78} rx={width * 0.46} ry={height * 0.55} fill="url(#gb)" />
        <Ellipse cx={width * 0.62} cy={height * 0.12} rx={width * 0.3} ry={height * 0.3} fill="url(#gc)" />
      </Svg>
    </View>
  );
}

// Edge swipes over a full-screen app (as on Lomiri): in from the left edge
// opens the launcher, in from the right edge the switcher. The strips are
// the shell's (shellRegions), so a touch starting there never reaches the
// app; the action fires once the finger has come EDGE_TRIGGER px in.
const EDGE = 14;
const EDGE_TRIGGER = 56;
function EdgeSwipe({ side, top, bottom, onSwipe }) {
  const cb = useRef(onSwipe);
  cb.current = onSwipe;
  const pan = useMemo(() => {
    let fired = false;
    const inward = (g) => (side === 'left' ? g.dx : -g.dx);
    return PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: () => {
        fired = false;
      },
      onPanResponderMove: (e, g) => {
        if (!fired && inward(g) > EDGE_TRIGGER && Math.abs(g.dx) > Math.abs(g.dy)) {
          fired = true;
          wm.haptic();
          cb.current();
        }
      },
      onPanResponderRelease: (e, g) => {
        // A quick flick that ended before the trigger distance counts too.
        if (!fired && inward(g) > EDGE_TRIGGER / 2 && (side === 'left' ? g.vx : -g.vx) > 0.5) {
          wm.haptic();
          cb.current();
        }
      },
    });
  }, [side]);
  return <View {...pan.panHandlers} style={{ position: 'absolute', top, bottom, width: EDGE, [side]: 0 }} />;
}

// The volume buttons' indicator: a pill under the top bar with the level,
// for 1.5 s after the last press (jsui-wl sets the volume itself).
function VolumeOsd({ screen, t }) {
  const [level, setLevel] = useState(null);
  const v = useRef(new Animated.Value(0)).current;
  const timer = useRef(null);
  useHostMessage('wm:volumeKey', (msg) => {
    setLevel(msg.level);
    setSetting('volume', msg.level);
    setSetting('muted', false);
    Animated.timing(v, { toValue: 1, duration: 120, useNativeDriver: true }).start();
    clearTimeout(timer.current);
    timer.current = setTimeout(() => {
      Animated.timing(v, { toValue: 0, duration: 200, useNativeDriver: true }).start(({ finished }) => finished && setLevel(null));
    }, 1500);
  });
  useEffect(() => () => clearTimeout(timer.current), []);
  if (level == null) return null;
  return (
    <Animated.View pointerEvents="none" style={[styles.osd, { top: screen.topBar + 14, left: (screen.width - 320) / 2, backgroundColor: t.panel, borderColor: t.panelLine, opacity: v }]}>
      <Icon name={level === 0 ? 'mute' : 'volume'} size={22} color={t.text} />
      <View style={[styles.osdTrack, { backgroundColor: t.panelRaised }]}>
        <View style={[styles.osdFill, { width: `${Math.round(level * 100)}%`, backgroundColor: t.accent }]} />
      </View>
    </Animated.View>
  );
}

// An app being started: a window-sized card with the app's icon and a
// spinner, grown out of the icon it was launched from, until the app's own
// window is up (it then fades away under it).
function Spinner({ color, track }) {
  const v = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const a = Animated.loop(Animated.timing(v, { toValue: 1, duration: 800, easing: Easing.linear, useNativeDriver: true }));
    a.start();
    return () => a.stop();
  }, [v]);
  const rotate = v.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '360deg'] });
  return <Animated.View style={[styles.spinner, { borderColor: track, borderTopColor: color, transform: [{ rotate }] }]} />;
}

function LaunchSplash({ launch, screen, t, bottom, done, onGone }) {
  const W = screen.width;
  const top = screen.topBar;
  const H = screen.height - top - bottom;
  const grow = useRef(new Animated.Value(0)).current;
  const fade = useRef(new Animated.Value(1)).current;
  const from = useRef(undefined);
  if (from.current === undefined) from.current = takeLaunchOrigin(launch.app.id);
  useEffect(() => {
    Animated.timing(grow, { toValue: 1, duration: from.current ? 320 : 200, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, [grow]);
  useEffect(() => {
    if (!done) return;
    Animated.timing(fade, { toValue: 0, duration: 180, easing: Easing.in(Easing.quad), useNativeDriver: true }).start(() => onGone(launch.key));
  }, [done, fade, onGone, launch.key]);
  const o = from.current;
  const tf = o
    ? [
        { translateX: grow.interpolate({ inputRange: [0, 1], outputRange: [o[0] + o[2] / 2 - W / 2, 0] }) },
        { translateY: grow.interpolate({ inputRange: [0, 1], outputRange: [o[1] + o[3] / 2 - (top + H / 2), 0] }) },
        { scaleX: grow.interpolate({ inputRange: [0, 1], outputRange: [o[2] / W, 1] }) },
        { scaleY: grow.interpolate({ inputRange: [0, 1], outputRange: [o[3] / H, 1] }) },
      ]
    : [{ scale: grow.interpolate({ inputRange: [0, 1], outputRange: [0.94, 1] }) }];
  const content = grow.interpolate({ inputRange: [0, 0.6, 1], outputRange: [0, 0, 1] });
  return (
    <Animated.View pointerEvents="none"
      style={[styles.splash, { top, width: W, height: H, backgroundColor: t.chrome, opacity: Animated.multiply(fade, o ? 1 : grow), transform: tf }]}>
      <Animated.View style={{ alignItems: 'center', opacity: content }}>
        <AppIcon app={launch.app} size={112} />
        <Text style={[styles.splashName, { color: t.text }]} numberOfLines={1}>{cleanName(launch.app.name)}</Text>
        <Spinner color={t.accent} track={t.panelLine} />
      </Animated.View>
    </Animated.View>
  );
}

// Big clock on the empty desktop: the lock screen's clock, in the same place,
// so unlocking to an empty desktop doesn't move it. The status bar leaves its
// own time out meanwhile (TopBar `quiet`).
function DesktopClock({ screen, t }) {
  const now = useClock();
  return (
    <View pointerEvents="none" style={[styles.deskClock, { top: screen.height * 0.18 }]}>
      <Text style={[styles.deskTime, { color: t.name === 'dark' ? '#fff' : 'rgba(20,24,60,0.85)' }]}>{timeText(now)}</Text>
      <Text style={[styles.deskDate, { color: t.name === 'dark' ? 'rgba(255,255,255,0.85)' : 'rgba(20,24,60,0.6)' }]}>{dateText(now)}</Text>
    </View>
  );
}

// Where a dragged window will land: `preview` is the finger ({x, y}, native
// values) while a window that can snap is dragged; each target fades in
// natively as the finger reaches its edge (the same edges Window.jsx's
// release decides by).
function SnapPreview({ preview, screen, t, bottom }) {
  const parts = useMemo(() => {
    if (!preview) return null;
    const step = (v, edge, below) =>
      v.interpolate({ inputRange: [edge, edge + 1], outputRange: below ? [1, 0] : [0, 1], extrapolate: 'clamp' });
    const max = step(preview.y, screen.topBar + 4, true);
    const notMax = Animated.subtract(1, max);
    return {
      max,
      left: Animated.multiply(step(preview.x, 6, true), notMax),
      right: Animated.multiply(step(preview.x, screen.width - 7, false), notMax),
    };
  }, [preview, screen.topBar, screen.width]);
  if (!parts) return null;
  const top = screen.topBar + 6;
  const h = screen.height - screen.topBar - bottom - 12;
  const half = (screen.width - 18) / 2;
  const look = { borderColor: t.accent, backgroundColor: t.name === 'dark' ? 'rgba(124,140,255,0.18)' : 'rgba(75,92,255,0.14)' };
  return (
    <>
      <Animated.View pointerEvents="none" style={[styles.snap, look, { left: 6, top, width: screen.width - 12, height: h, opacity: parts.max }]} />
      <Animated.View pointerEvents="none" style={[styles.snap, look, { left: 6, top, width: half, height: h, opacity: parts.left }]} />
      <Animated.View pointerEvents="none" style={[styles.snap, look, { left: 12 + half, top, width: half, height: h, opacity: parts.right }]} />
    </>
  );
}

// Split view: the seam between a left- and a right-snapped window. Drag it to
// share the screen differently (25-75 %).
function SplitDivider({ left, right, screen, t, bottom }) {
  // The grip follows the finger natively, clamped to 25-75 % of the screen.
  // The two windows follow live: the ratio goes to the host at most every
  // LIVE_MS while the finger moves (apps relayout at that rate, not at the
  // finger's), and once more on release.
  const LIVE_MS = 50;
  const [grabbed, setGrabbed] = useState(null); // the seam where the drag began, while held
  const [settling, setSettling] = useState(null); // where it was let go, until the windows follow
  const drag = useNativeDragX();
  const seam = (left.x + left.w + right.x) / 2;
  const base = grabbed != null ? grabbed : seam;
  const anchor = useAnchor('split.divider');
  const shift = useMemo(
    () => drag.dx.interpolate({
      inputRange: [screen.width * 0.25 - base, screen.width * 0.75 - base],
      outputRange: [screen.width * 0.25 - base, screen.width * 0.75 - base],
      extrapolate: 'clamp',
    }),
    [drag, base, screen.width],
  );
  useEffect(() => {
    if (grabbed == null) setSettling(null);
  }, [seam, grabbed]);
  const live = useRef({ at: 0, sent: null, timer: null, last: null });
  useEffect(() => () => clearTimeout(live.current.timer), []);
  // The responder must survive the live resizes (seam changes mid-drag), so
  // it reads the current seam and width through a ref.
  const cur = useRef(null);
  cur.current = { seam, width: screen.width };
  const pan = useMemo(() => {
    let start = 0;
    let width = 1;
    const clampX = (g) => Math.max(width * 0.25, Math.min(width * 0.75, start + g.dx));
    const send = (nx) => {
      const L = live.current;
      L.at = Date.now();
      if (L.sent != null && Math.abs(nx - L.sent) < 1) return;
      L.sent = nx;
      wm.splitRatio(nx / width);
    };
    const end = (g) => {
      const L = live.current;
      clearTimeout(L.timer);
      L.timer = null;
      const nx = clampX(g);
      setSettling(nx);
      setGrabbed(null);
      drag.on.setValue(0);
      if (Math.abs(g.dx) > 1) send(nx);
    };
    return PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: () => {
        start = cur.current.seam;
        width = cur.current.width;
        live.current.sent = start;
        drag.on.setValue(1);
        setGrabbed(start);
      },
      onPanResponderMove: (e, g) => {
        const L = live.current;
        L.last = clampX(g);
        const wait = LIVE_MS - (Date.now() - L.at);
        if (wait <= 0) send(L.last);
        else if (!L.timer) {
          L.timer = setTimeout(() => {
            L.timer = null;
            send(L.last);
          }, wait);
        }
      },
      onPanResponderRelease: (e, g) => end(g),
      onPanResponderTerminate: (e, g) => end(g),
    });
  }, [drag]);
  const held = grabbed != null;
  const x = held ? grabbed : settling;
  const cx = x != null ? x : seam;
  const top = screen.topBar;
  const h = screen.height - screen.topBar - bottom;
  return (
    <Animated.View {...pan.panHandlers} {...drag.handlers} ref={anchor.ref} onLayout={anchor.onLayout}
      style={{ position: 'absolute', left: cx - 16, top, width: 32, height: h, alignItems: 'center', justifyContent: 'center', transform: [{ translateX: held ? shift : 0 }] }}>
      <View style={{ width: x != null ? 8 : 6, height: x != null ? 64 : 44, borderRadius: 4, backgroundColor: x != null ? t.accent : t.name === 'dark' ? 'rgba(255,255,255,0.7)' : 'rgba(20,24,60,0.5)' }} />
    </Animated.View>
  );
}

// -------------------------------------------------------------------- Shell

let noteId = 1;
const nowText = () => {
  const d = new Date();
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
};

function Shell() {
  const screen = useScreen();
  const dims = useWindowDimensions();
  const scr = useMemo(() => ({ ...screen, width: dims.width, height: dims.height }), [screen, dims.width, dims.height]);
  const settings = useSettings();
  const system = useSystem();
  // The sliders start at the device's backlight and volume (published once
  // by the host), not at whatever the setting last held.
  const adopted = useRef({});
  useEffect(() => {
    if (system.backlight != null && system.backlight >= 0 && !adopted.current.backlight) {
      adopted.current.backlight = true;
      setSetting('brightness', system.backlight);
    }
    if (system.volume && !adopted.current.volume) {
      adopted.current.volume = true;
      setSetting('volume', system.volume.level);
      setSetting('muted', !!system.volume.muted);
    }
  }, [system.backlight, system.volume]);
  const apps = useApps();
  const popups = usePopups();
  const { list: windows, active, map } = useWindows();
  const t = settings.darkMode ? dark : light;

  const [panel, setPanel] = useState(null); // launcher | overview | quick | calendar
  // A panel that was just closed stays mounted while it animates out.
  const [panels, setPanels] = useState({ cur: null, closing: null });
  if (panels.cur !== panel) setPanels({ cur: panel, closing: panels.cur || (panels.closing !== panel ? panels.closing : null) });
  const closingPanel = panels.closing !== panel ? panels.closing : null;
  const panelClosed = useCallback(() => setPanels((p) => ({ ...p, closing: null })), []);
  const [winMenu, setWinMenu] = useState(null);
  const [peek, setPeek] = useState(false);
  const [locked, setLocked] = useState(false);
  const [preview, setPreview] = useState(null);
  const [dragging, setDragging] = useState(null);
  const [dockMenu, setDockMenu] = useState(null);
  const [dockRect, setDockRect] = useState(null);
  const [oskRect, setOskRect] = useState(null);
  // The on-screen keyboard: on by the user (settings.osk) or while a text
  // field in the focused app wants it (wm:textInput).
  const [autoOsk, setAutoOsk] = useState(false);
  const oskOn = settings.osk || autoOsk;
  useHostMessage('wm:textInput', (msg) => setAutoOsk(!!msg.show));
  const hideOsk = useCallback(() => {
    if (autoOsk) wm.textInputClosed();
    setAutoOsk(false);
  }, [autoOsk]);
  // Apps make room for it (the host shrinks their work area).
  const oskHeight = oskOn && oskRect ? oskRect.height : 0;
  useEffect(() => wm.osk(oskHeight), [oskHeight]);
  const [notes, setNotes] = useState(() => [
    { id: noteId++, icon: 'tablet', title: 'Welcome to the JSUI desktop', body: 'Swipe up from the bottom edge for the dock, further for your recent apps.', time: nowText() },
  ]);
  const [toasts, setToasts] = useState([]);
  // Locked, a notification only counts up the lock screen's pill: no toast.
  const lockedRef = useRef(false);
  lockedRef.current = locked;

  const notify = useCallback((n) => {
    const note = { id: noteId++, time: nowText(), ...n };
    setNotes((l) => [note, ...l].slice(0, 30));
    if (!settings.doNotDisturb && !lockedRef.current) {
      setToasts((l) => [...l, note].slice(-3));
    }
  }, [settings.doNotDisturb]);
  const toastDone = useCallback((id) => setToasts((l) => l.filter((x) => x.id !== id)), []);
  const dismissNote = useCallback((id) => setNotes((l) => l.filter((x) => x.id !== id)), []);

  // Windows that disappeared animate out from their last state: the same
  // Window, kept mounted with `closing` (mounting a copy took the JS thread
  // a render, ~150 ms on the Tab A7, with neither on screen). closingNow has
  // them from the render that drops them; the effect keeps them in
  // `closing` until done.
  const prev = useRef(new Map());
  const [closing, setClosing] = useState([]);
  const closingNow = useMemo(() => {
    const out = [...closing];
    for (const [id, w] of prev.current) if (!map[id] && w.ready && !w.minimized && !out.some((c) => c.id === w.id)) out.push(w);
    return out;
  }, [map, closing]);
  useEffect(() => {
    const gone = [];
    for (const [id, w] of prev.current) if (!map[id] && w.ready && !w.minimized) gone.push(w);
    if (gone.length) setClosing((c) => [...c, ...gone.filter((w) => !c.some((x) => x.id === w.id))]);
    prev.current = new Map(Object.entries(map));
    for (const w of gone) prev.current.delete(String(w.id));
  }, [map]);
  const doneClosing = useCallback((id) => {
    setClosing((c) => c.filter((w) => w.id !== id));
    wm.release(id);
  }, []);

  const activeWin = map[active];
  const fullscreen = !!(activeWin && activeWin.fullscreen && !activeWin.minimized);
  const visible = windows.filter((w) => !w.minimized);
  const splitL = visible.find((w) => w.snap === 'left');
  const splitR = visible.find((w) => w.snap === 'right');
  // Split view counts while the pair is what's on top (the stack's last normal window).
  const topNormal = [...visible].reverse().find((w) => w.type === 'normal' && !w.transientFor);
  const split = splitL && splitR && !fullscreen && topNormal && (topNormal === splitL || topNormal === splitR)
    ? { left: splitL, right: splitR } : null;
  // Tablet mode with an app on screen (or one starting, launching.js): apps
  // reach the bottom edge, the dock hides behind the home indicator.
  const launching = useLaunching();
  const appMode = settings.tabletMode && (launching.length > 0 || visible.some((w) => w.type === 'normal' && (w.maximized || w.snap)));
  const dockShown = !appMode || peek || panel === 'launcher' || panel === 'overview';
  const bottom = appMode ? scr.homeBar || 24 : scr.dock;
  const solidBar = visible.some((w) => w.maximized || w.snap);
  const onPeek = useCallback(() => setPeek(true), []);
  useEffect(() => {
    if (!peek) return undefined;
    const id = setTimeout(() => setPeek(false), 4000);
    return () => clearTimeout(id);
  }, [peek, active]);
  useEffect(() => setPeek(false), [active, panel]);

  // Hotkeys from the host: Super toggles the launcher, Alt+Tab the overview.
  // An app that never showed a window (jsui-wl ended it).
  // Apps' notifications (org.freedesktop.Notifications, the host's
  // jsui-notifyd): a toast with the app's icon unless Do Not Disturb is on
  // (urgent ones show anyway), and an entry in the list. A replaced one
  // updates its entry; one the app withdraws goes.
  const appNotes = useRef(new Map()); // the app's id -> our note id
  useHostMessage('wm:notification', (msg) => {
    const ours = appNotes.current.get(msg.id);
    if (msg.closed) {
      if (ours == null) return;
      appNotes.current.delete(msg.id);
      setNotes((l) => l.filter((x) => x.id !== ours));
      setToasts((l) => l.filter((x) => x.id !== ours));
      return;
    }
    const d = (msg.desktop || '').toLowerCase();
    const n = (msg.app || '').toLowerCase();
    const app = apps.find((a) => (d && (a.id.toLowerCase() === d || a.id.toLowerCase().startsWith(d.split('_')[0]))) ||
      (n && cleanName(a.name).toLowerCase() === n));
    const note = {
      app,
      icon: 'bell',
      title: msg.title || (app ? cleanName(app.name) : msg.app) || 'Notification',
      body: msg.title && msg.body ? msg.body : msg.title ? '' : msg.body,
    };
    if (ours != null) {
      setNotes((l) => l.map((x) => (x.id === ours ? { ...x, ...note } : x)));
      setToasts((l) => l.map((x) => (x.id === ours ? { ...x, ...note } : x)));
      return;
    }
    const id = noteId++;
    appNotes.current.set(msg.id, id);
    const full = { id, time: nowText(), notifyId: msg.id, ...note };
    setNotes((l) => [full, ...l].slice(0, 30));
    if (!lockedRef.current && (!settings.doNotDisturb || msg.urgency >= 2)) setToasts((l) => [...l, full].slice(-3));
  });

  const endLaunchExec = useRef(() => {});
  useHostMessage('wm:launchFailed', (msg) => {
    endLaunchExec.current(msg.exec);
    notify({ icon: 'bell', title: `${msg.name} didn't start`, body: 'It showed no window and was closed.' });
  });

  useHostMessage('wm:hotkey', (msg) => {
    if (locked) return;
    if (msg.name === 'super') setPanel((p) => (p === 'launcher' ? null : 'launcher'));
    else if (msg.name === 'switch') setPanel((p) => (p === 'overview' ? null : 'overview'));
  });

  // Close panels with nothing to show.
  useEffect(() => {
    if (panel === 'overview' && !visible.length) {
      const id = setTimeout(() => setPanel(null), 600);
      return () => clearTimeout(id);
    }
  }, [panel, visible.length]);

  // The input the shell takes above the windows.
  // Edge swipes (EdgeSwipe) while a full-screen app has the screen.
  const edges = appMode && !fullscreen && !locked && !panel;
  const regions = useMemo(() => {
    const W = scr.width;
    const H = scr.height;
    if (locked || panel || dockMenu || winMenu) return [[0, 0, W, H]];
    const r = [];
    if (!fullscreen) {
      r.push([0, 0, W, scr.topBar]);
      if (!oskOn && dockShown && dockRect) r.push([dockRect.x, H - scr.dock + dockRect.y, dockRect.width, dockRect.height]);
      else if (!oskOn && dockShown) r.push([0, H - scr.dock, W, scr.dock]);
      if (!oskOn && appMode) r.push([0, H - bottom, W, bottom]);
    }
    if (edges) {
      r.push([0, scr.topBar, EDGE, H - scr.topBar - bottom]);
      r.push([W - EDGE, scr.topBar, EDGE, H - scr.topBar - bottom]);
    }
    if (split) {
      const cx = (split.left.x + split.left.w + split.right.x) / 2;
      r.push([cx - 16, scr.topBar, 32, H - scr.topBar - bottom]);
    }
    if (oskOn && oskRect) r.push([oskRect.x, oskRect.y, oskRect.width, oskRect.height]);
    // Toasts take a tap (it puts them away), not the app under them.
    if (toasts.length) r.push([(W - 400) / 2, scr.topBar + 10, 400, toasts.length * 92]);
    return r;
  }, [toasts.length, locked, panel, dockMenu, winMenu, fullscreen, oskOn, dockRect, oskRect, scr, dockShown, appMode, bottom, edges, split && split.left.x + split.left.w, split && split.right.x]);
  const lastRegions = useRef('');
  useEffect(() => {
    const json = JSON.stringify(regions);
    if (json !== lastRegions.current) {
      lastRegions.current = json;
      wm.shellRegions(regions);
    }
  }, [regions]);

  // The launcher is painted once, invisibly, a few seconds after start (see
  // Launcher in panels.jsx), so its first open is as cheap as the next ones.
  const [warmLauncher, setWarmLauncher] = useState(false);
  const appsReady = apps.length > 0;
  useEffect(() => {
    if (!appsReady) return undefined;
    const id = setTimeout(() => setWarmLauncher(true), 4000);
    return () => clearTimeout(id);
  }, [appsReady]);

  // A tapped notification: its app's window to the front, else the app started.
  const openNote = useCallback((n) => {
    // The app hears of it (ActionInvoked "default", then closed): the
    // notification is done, so it leaves the list too.
    if (n.notifyId != null) {
      wm.notificationAction(n.notifyId);
      appNotes.current.delete(n.notifyId);
      setNotes((l) => l.filter((x) => x.id !== n.id));
    }
    if (!n.app) return;
    const w = [...windows].reverse().find((x) => x.appId === n.app.id && x.type === 'normal');
    if (w) {
      if (w.minimized) wm.restore(w.id);
      wm.focus(w.id);
    } else {
      startLaunch(n.app, null);
    }
  }, [windows]);

  const winsRef = useRef(windows);
  winsRef.current = windows;
  const launch = useCallback((app) => {
    wm.haptic();
    startLaunch(app, `app.${app.id}`, winsRef.current.some((w) => w.appId === app.id && w.type === 'normal'));
  }, []);

  // Loading windows of apps being started (launching.js): each goes once a
  // new window of its app is ready to show (or any new app window, for the
  // only one pending: an app's window may not name its desktop file), when
  // the host says the launch ended, or after LAUNCH_TIMEOUT_MS.
  const [launchDone, setLaunchDone] = useState({}); // key -> true while it fades
  const knownWins = useRef(null);
  if (knownWins.current == null) knownWins.current = new Set(windows.map((w) => w.id));
  useEffect(() => {
    if (!launching.length) {
      knownWins.current = new Set(windows.map((w) => w.id));
      return;
    }
    const fresh = windows.filter((w) => w.ready && w.type === 'normal' && !w.transientFor && !knownWins.current.has(w.id));
    if (!fresh.length) return;
    const done = {};
    for (const w of fresh) {
      const l = launching.find((x) => x.app.id === w.appId && !launchDone[x.key] && !done[x.key])
        || (launching.length === 1 && !launchDone[launching[0].key] && !done[launching[0].key] && !apps.some((a) => a.id === w.appId) ? launching[0] : null);
      if (l) {
        done[l.key] = true;
        knownWins.current.add(w.id);
      }
    }
    if (Object.keys(done).length) setLaunchDone((d) => ({ ...d, ...done }));
  }, [windows, launching, launchDone, apps]);
  useEffect(() => {
    if (!launching.length) return undefined;
    const oldest = Math.min(...launching.map((l) => l.at));
    const id = setTimeout(() => {
      const now = Date.now();
      const done = {};
      for (const l of launching) if (now - l.at >= LAUNCH_TIMEOUT_MS) done[l.key] = true;
      setLaunchDone((d) => ({ ...d, ...done }));
    }, Math.max(0, oldest + LAUNCH_TIMEOUT_MS - Date.now()) + 50);
    return () => clearTimeout(id);
  }, [launching]);
  const launchGone = useCallback((key) => {
    endLaunch((l) => l.key === key);
    setLaunchDone((d) => {
      const n = { ...d };
      delete n[key];
      return n;
    });
  }, []);
  const endByExec = useCallback((exec) => {
    const done = {};
    for (const l of launching) if (l.app.exec === exec) done[l.key] = true;
    if (Object.keys(done).length) setLaunchDone((d) => ({ ...d, ...done }));
  }, [launching]);
  useHostMessage('wm:launchEnded', (msg) => endByExec(msg.exec));
  endLaunchExec.current = endByExec;

  // The launcher and the switcher draw over a blurred snapshot of the screen
  // as it was when they opened (see Scrim in panels.jsx), dropped once they
  // are gone.
  const fullPanel = panel === 'launcher' || panel === 'overview';
  const fullShown = fullPanel || closingPanel === 'launcher' || closingPanel === 'overview';
  useEffect(() => {
    if (fullPanel) wm.snapshot(true);
  }, [fullPanel]);
  useEffect(() => {
    if (!fullShown) wm.snapshot(false);
  }, [fullShown]);

  const closePanel = useCallback(() => setPanel(null), []);
  const onDragging = useCallback((id, on) => setDragging(on ? id : null), []);
  const onWinMenu = useCallback((win) => setWinMenu(win.id), []);
  const closeWinMenu = useCallback(() => setWinMenu(null), []);
  const overviewUp = panel === 'overview' || closingPanel === 'overview';
  const panelEl = (name, open) => {
    const common = { key: name, screen: scr, open, onClose: closePanel, onClosed: open ? undefined : panelClosed };
    if (name === 'overview') return <Overview {...common} windows={visible} active={active} apps={apps} />;
    if (name === 'quick') {
      return (
        <QuickSettings {...common} settings={settings} system={system} windows={windows}
          onLock={() => { setPanel(null); setLocked(true); }} onReload={() => DevSettings.reload()} />
      );
    }
    if (name === 'calendar') return <CalendarPanel {...common} notifications={notes} onClear={() => setNotes([])} onDismiss={dismissNote} onOpen={openNote} />;
    return null;
  };

  return (
    <ThemeContext.Provider value={t}>
      <View style={styles.root}>
        <Wallpaper width={scr.width} height={scr.height} t={t} />
        {/* Mounted under panels too: mounting it again as one closes laid out and
            painted its big paragraph afresh in the first frame of the close. */}
        {!visible.length && <DesktopClock screen={scr} t={t} />}
        {split && !overviewUp && <View pointerEvents="none" style={{ position: 'absolute', left: split.left.x + split.left.w - 1, width: split.right.x - split.left.x - split.left.w + 2, top: split.left.y - scr.titleBar, height: split.left.h + scr.titleBar, backgroundColor: t.name === 'dark' ? '#000' : t.wallTop }} />}
        {[...windows, ...closingNow.filter((c) => !map[c.id])].map((w) =>
          w.type === 'dock' || w.type === 'desktop' ? null : (
            <View key={w.id} pointerEvents="box-none" style={[StyleSheet.absoluteFill, { opacity: overviewUp ? 0 : 1 }]}>
            <Window
              win={w}
              active={w.id === active}
              screen={scr}
              apps={apps}
              hidden={overviewUp || !w.ready}
              onPreview={setPreview}
              onDragging={onDragging}
              tablet={settings.tabletMode}
              onMenu={onWinMenu}
              menuOpen={winMenu === w.id}
              onMenuClose={closeWinMenu}
              closing={!map[w.id]}
              onClosed={doneClosing}
            />
            </View>
          ),
        )}
        {split && !overviewUp && <SplitDivider left={split.left} right={split.right} screen={scr} t={t} bottom={bottom} />}
        <SnapPreview preview={preview} screen={scr} t={t} bottom={bottom} />
        {edges && <EdgeSwipe side="left" top={scr.topBar} bottom={bottom} onSwipe={() => setPanel('launcher')} />}
        {edges && <EdgeSwipe side="right" top={scr.topBar} bottom={bottom} onSwipe={() => setPanel('overview')} />}
        {!fullscreen && (
          <TopBar screen={scr} solid={solidBar} panel={panel} setPanel={setPanel} settings={settings} system={system} quiet={!visible.length && !panel} />
        )}
        {!fullscreen && !oskOn && appMode && !dockShown && (
          <HomeBar screen={scr} windows={windows} setPanel={setPanel} onPeek={onPeek} />
        )}
        {!fullscreen && !oskOn && (
          <Dock
            screen={scr}
            apps={apps}
            windows={windows}
            active={active}
            settings={settings}
            panel={panel}
            setPanel={setPanel}
            onMenu={setDockMenu}
            onMeasure={setDockRect}
            shown={dockShown}
          />
        )}
        {/* The launcher stays mounted (hidden while closed): opening it then
            paints a laid-out grid with its text shaped and icons decoded,
            not a fresh tree (a ~50 ms frame on the Tab A7). */}
        <Launcher screen={scr} apps={apps} settings={settings} onLaunch={launch} open={panel === 'launcher'} warm={warmLauncher}
          onClose={closePanel} onClosed={panelClosed} />
        {[closingPanel, panel].filter((n) => n && n !== 'launcher').map((name) => panelEl(name, name === panel))}
        {/* Over the launcher: the card grows out of the tapped icon as the
            launcher fades. Hidden while the launcher or the switcher is up. */}
        {launching.length > 0 && (
          <View pointerEvents="none" style={[StyleSheet.absoluteFill, { opacity: overviewUp || panel === 'launcher' ? 0 : 1 }]}>
            {launching.map((l) => (
              <LaunchSplash key={l.key} launch={l} screen={scr} t={t} bottom={bottom} done={!!launchDone[l.key]} onGone={launchGone} />
            ))}
          </View>
        )}
        {/* The window menu itself is an overlay of its button (WindowMenu.jsx): taps elsewhere close it. */}
        {winMenu != null && <Pressable style={StyleSheet.absoluteFill} onPress={closeWinMenu} />}
        {dockMenu && (
          <DockMenu item={dockMenu} anchor={dockRect && { x: dockRect.x + dockRect.width / 2, width: 0 }} onClose={() => setDockMenu(null)} />
        )}
        {oskOn && <Keyboard screen={scr} onMeasure={setOskRect} onHide={hideOsk} />}
        {locked && (
          <LockScreen screen={scr} notifications={notes} onUnlock={() => { wm.haptic(18); setLocked(false); }}>
            <Wallpaper width={scr.width} height={scr.height} t={t} />
          </LockScreen>
        )}
        <Toasts screen={scr} toasts={toasts} onDone={toastDone} onOpen={openNote} />
        <VolumeOsd screen={scr} t={t} />
        {Object.values(popups).map((p) =>
          p.ready ? (
            <HostTexture key={p.id} name={`win:${p.id}`} pointerEvents="none"
              style={{ position: 'absolute', left: p.x, top: p.y, width: p.w, height: p.h, boxShadow: '0px 6px 20px rgba(0,0,0,0.35)' }} />
          ) : null,
        )}
        {/* Brightness without a backlight to set dims the picture instead. */}
        {((settings.brightness < 1 && system.backlight == null) || settings.nightLight) && (
          <View pointerEvents="none" style={[StyleSheet.absoluteFill, { backgroundColor: settings.nightLight ? `rgba(60,30,0,${0.18 + (system.backlight == null ? (1 - settings.brightness) * 0.6 : 0)})` : `rgba(0,0,0,${(1 - settings.brightness) * 0.75})` }]} />
        )}
      </View>
    </ThemeContext.Provider>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, overflow: 'hidden' },
  deskClock: { position: 'absolute', left: 0, right: 0, alignItems: 'center' },
  // The same type as the lock screen's clock (panels.jsx lockTime / lockDate).
  deskTime: { fontFamily: font, fontSize: 132, fontWeight: '700', letterSpacing: -4 },
  deskDate: { fontFamily: font, fontSize: 26, fontWeight: '500' },
  snap: { position: 'absolute', borderRadius: 18, borderWidth: 3 },
  osd: { position: 'absolute', width: 320, height: 56, borderRadius: 28, borderWidth: 1, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 18 },
  osdTrack: { flex: 1, height: 8, borderRadius: 4, marginLeft: 14, overflow: 'hidden' },
  osdFill: { height: 8, borderRadius: 4 },
  splash: { position: 'absolute', left: 0, borderRadius: 18, alignItems: 'center', justifyContent: 'center', overflow: 'hidden' },
  splashName: { fontFamily: font, fontSize: 22, fontWeight: '600', marginTop: 22, marginBottom: 30 },
  spinner: { width: 34, height: 34, borderRadius: 17, borderWidth: 3.5 },
});

AppRegistry.registerComponent('main', () => Shell);
