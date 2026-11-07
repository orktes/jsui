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
import { playNotify, playTick } from './sounds.js';
import { remeasureAnchors, takeLaunchOrigin, useAnchor } from './anchors.js';
import { AppIcon } from './AppIcon.jsx';
import { ActionMenu } from './WindowMenu.jsx';
import { LAUNCH_TIMEOUT_MS, endLaunch, startLaunch, useLaunching } from './launching.js';
import { useNativeDragX, useNativeDragY } from './drag.js';
import { Keyboard } from './keyboard.jsx';
import { AppTile, CalendarPanel, Launcher, LockScreen, Overview, QuickSettings, Toasts } from './panels.jsx';
import { ThemeContext, dark, font, light, radius } from './theme.js';
import { Window } from './Window.jsx';
import { appName, cleanName, homeApps, pinHome, setSetting, useApps, usePopups, useScreen, useSettings, useSystem, useWindows, wm } from './wm.js';

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

// The volume buttons' indicator, for 1.5 s after the last press (jsui-wl sets
// the volume itself): Quick Settings' volume slider with a shadow, in the
// toasts' place and width.
function VolumeOsd({ screen, t }) {
  const [level, setLevel] = useState(null);
  const v = useRef(new Animated.Value(0)).current;
  const timer = useRef(null);
  useHostMessage('wm:volumeKey', (msg) => {
    if (msg.level > 0) playTick();
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
    <Animated.View pointerEvents="none" style={[styles.osd, { top: screen.topBar + 10, left: (screen.width - 400) / 2, backgroundColor: t.panelRaised, boxShadow: `0px 12px 32px ${t.shadow}`, opacity: v }]}>
      <View style={[styles.osdFill, { width: Math.max(48, level * 400), backgroundColor: t.tileOn }]} />
      <View style={styles.osdIcon}>
        <Icon name={level === 0 ? 'mute' : 'volume'} size={22} color="#fff" />
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

function LaunchSplash({ launch, screen, t, done, onGone }) {
  const W = screen.width;
  const top = screen.topBar;
  const H = screen.height - top; // to the bottom edge, as the app's window (under the home indicator)
  const grow = useRef(new Animated.Value(0)).current;
  const fade = useRef(new Animated.Value(1)).current;
  const from = useRef(undefined);
  if (from.current === undefined) from.current = takeLaunchOrigin(launch.app.id);
  useEffect(() => {
    Animated.timing(grow, { toValue: 1, duration: from.current ? 320 : 200, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, [grow]);
  useEffect(() => {
    if (!done) return;
    // Two frames first: the app's window gets its first frames drawn under the
    // card (those are slow), and the fade then runs on smooth ones.
    let raf = requestAnimationFrame(() => {
      raf = requestAnimationFrame(() => {
        raf = 0;
        Animated.timing(fade, { toValue: 0, duration: 220, easing: Easing.inOut(Easing.quad), useNativeDriver: true }).start(() => onGone(launch.key));
      });
    });
    return () => raf && cancelAnimationFrame(raf);
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
      {/* The status bar names the app (TopBar `app`), as it will over the
          app's window: here just its icon and a spinner. */}
      <Animated.View style={[styles.splashBody, { opacity: content }]}>
        <AppIcon app={launch.app} size={96} />
        <View style={{ height: 32 }} />
        <Spinner color={t.accent} track={t.panelLine} />
      </Animated.View>
    </Animated.View>
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
  // Portrait stacks the pair (the host's snapSlot): the left edge means the top half.
  const tall = screen.height > screen.width;
  const half = ((tall ? h : screen.width - 12) - 6) / 2;
  const first = tall ? { left: 6, top, width: screen.width - 12, height: half } : { left: 6, top, width: half, height: h };
  const second = tall ? { left: 6, top: top + half + 6, width: screen.width - 12, height: half } : { left: 12 + half, top, width: half, height: h };
  const look = { borderColor: t.accent, backgroundColor: t.name === 'dark' ? 'rgba(124,140,255,0.18)' : 'rgba(75,92,255,0.14)' };
  return (
    <>
      <Animated.View pointerEvents="none" style={[styles.snap, look, { left: 6, top, width: screen.width - 12, height: h, opacity: parts.max }]} />
      <Animated.View pointerEvents="none" style={[styles.snap, look, first, { opacity: parts.left }]} />
      <Animated.View pointerEvents="none" style={[styles.snap, look, second, { opacity: parts.right }]} />
    </>
  );
}

// Split view's two panes, under the windows: a window that keeps a size of
// its own (Clock ignores the size it is given) sits on its pane, not on a
// hole of wallpaper, and the seam between them is the wallpaper's darkest
// colour.
// A split window's pane (the host's `slot`: the window itself may sit
// centred in it, smaller), or the window's own rect.
const paneOf = (w) => (w.slot ? { x: w.slot[0], y: w.slot[1], w: w.slot[2], h: w.slot[3] } : w);
// On a portrait screen the host stacks the pair (left on top, right below,
// each under its own title bar).
const stacked = (split) => {
  const l = paneOf(split.left);
  return paneOf(split.right).y >= l.y + l.h;
};
// Where the seam is: an x, or a y when stacked.
const seamOf = (split, screen) => {
  const l = paneOf(split.left);
  const r = paneOf(split.right);
  return stacked(split) ? (l.y + l.h + r.y - screen.titleBar) / 2 : (l.x + l.w + r.x) / 2;
};

function splitPanes(split, screen) {
  const left = paneOf(split.left);
  const right = paneOf(split.right);
  const tb = screen.titleBar;
  if (stacked(split)) {
    const gap = right.y - tb - left.y - left.h;
    const x = Math.min(left.x, right.x);
    const width = Math.max(left.x + left.w, right.x + right.w) - x;
    return {
      seam: { left: x, width, top: left.y + left.h - 1, height: gap + 2 },
      left: { left: x, width, top: left.y - tb, height: left.h + tb },
      right: { left: x, width, top: right.y - tb, height: right.h + tb },
    };
  }
  const seam = (left.x + left.w + right.x) / 2;
  const gap = right.x - left.x - left.w;
  const top = Math.min(left.y, right.y) - tb;
  const height = Math.max(left.y + left.h, right.y + right.h) - top;
  return {
    seam: { left: left.x + left.w - 1, width: gap + 2, top, height },
    left: { left: left.x, width: seam - gap / 2 - left.x, top, height },
    right: { left: seam + gap / 2, width: screen.width - left.x - seam - gap / 2, top, height },
  };
}

// Home's glance: the time in light type, the date, and the newest app
// notification of the last hour on one line (none: an empty line). Not the lock
// screen's bold clock, so home and lock tell apart; the status bar leaves its
// own time out meanwhile (TopBar `quiet`). A tap opens the notifications and
// calendar.
function HomeGlance({ t, note, apps, onPress, width }) {
  const now = useClock();
  // An app's notification as "App · text"; nothing (no line) otherwise.
  const app = note && (note.app || (note.appName && apps.find((a) => cleanName(a.name) === note.appName)));
  const source = note && (app ? shortName(app) : note.appName);
  const text = note && (note.body ? `${note.title}: ${note.body}` : note.title);
  const dark = t.name === 'dark';
  return (
    <Pressable onPress={onPress} style={[styles.glance, { width }]}>
      <Text style={[styles.glanceTime, { color: dark ? '#fff' : t.text }]}>{timeText(now)}</Text>
      <Text style={[styles.glanceDate, { color: dark ? 'rgba(255,255,255,0.9)' : t.text }]}>{dateText(now)}</Text>
      {/* Its line keeps its place when empty: home doesn't move as one comes and goes. */}
      <Text numberOfLines={1} style={[styles.glanceLine, { color: dark ? 'rgba(255,255,255,0.78)' : t.textDim }]}>
        {source ? <Text style={{ fontWeight: '500' }}>{`${source} · `}</Text> : null}
        {note ? text : ' '}
      </Text>
    </Pressable>
  );
}

// Home's names: short ones, never cut ("Morph Browser" -> "Browser").
const SHORT_NAMES = [[/morph-browser/, 'Browser'], [/system-settings/, 'Settings'], [/barcode/, 'Scanner'], [/filemanager|file-manager/, 'Files'], [/external/, 'Storage'], [/mediaplayer|media-player/, 'Media']];
function shortName(app) {
  const hit = SHORT_NAMES.find(([re]) => re.test(app.id));
  if (hit) return hit[1];
  const n = cleanName(app.name);
  return n.length > 11 ? n.split(' ')[0] : n;
}

// A shortcut on home: a 56 px icon over its short name. "All apps" is a
// circle (a control, not an app).
function Shortcut({ app, label, onPress, onLongPress, anchorName, menuOpen, menu, onMenuClose, t }) {
  const anchor = useAnchor(anchorName);
  return (
    <View>
      <Pressable ref={anchor.ref} onLayout={anchor.onLayout} onPress={onPress} onLongPress={onLongPress} delayLongPress={450}
        style={styles.shortcut} pressedStyle={{ transform: [{ scale: 0.92 }] }}>
        {app ? <AppIcon app={app} size={56} /> : (
          <View style={[styles.allApps, { backgroundColor: 'rgba(255,255,255,0.28)' }]}><Icon name="grid" size={26} color="#fff" strokeWidth={2.4} /></View>
        )}
        <Text numberOfLines={1} style={[styles.shortcutLabel, { color: t.name === 'dark' ? 'rgba(255,255,255,0.92)' : t.text }]}>{label}</Text>
      </Pressable>
      {menuOpen && menu && <ActionMenu rows={menu} onClose={onMenuClose} placement="top" />}
    </View>
  );
}

// Home: the glance and, under it, a glass card with the apps picked for home
// (at most 4) as shortcuts and "All apps" (a circle: a way
// to everything else, not a fifth app); one row, centred under the glance, in
// both orientations. The group sits a little above the middle. A long press
// on a shortcut unpins it; the launcher's long press pins.
const HOME_SLOT = { w: 104, h: 92 };
// The card's padding, less the slots' own margins around icon and label,
// so it shows as about one even 20 px inset.
const HOME_PAD = { top: 16, bottom: 12, side: 2 };
function Home({ screen, t, note, apps, settings, onLaunch, onCalendar, onAllApps }) {
  const picked = homeApps(settings, apps).slice(0, 4);
  const [menuFor, setMenuFor] = useState(null);
  const close = useCallback(() => setMenuFor(null), []);
  const menu = menuFor && [{ icon: 'close', label: 'Unpin from home screen', fn: () => pinHome(settings, apps, menuFor, false) }];
  const cardW = (picked.length + 1) * HOME_SLOT.w + 2 * HOME_PAD.side + 2; // + the hairline
  const groupH = 92 + 32 + 26 + 56 + HOME_SLOT.h + HOME_PAD.top + HOME_PAD.bottom;
  const top = Math.max(screen.topBar + 24, Math.round(screen.height * 0.45 - groupH / 2));
  return (
    <>
      <View pointerEvents="box-none" style={[styles.home, { top }]}>
        <HomeGlance t={t} note={note} apps={apps} onPress={onCalendar} width={Math.max(cardW, 480)} />
        <View style={[styles.homeCard, { width: cardW }]}>
          {picked.map((app) => (
            <Shortcut key={app.id} app={app} label={shortName(app)} t={t} anchorName={`home.${app.id}`}
              onPress={() => onLaunch(app, `home.${app.id}`)} onLongPress={() => setMenuFor(app.id)}
              menuOpen={menuFor === app.id} menu={menuFor === app.id ? menu : null} onMenuClose={close} />
          ))}
          <Shortcut label="All apps" t={t} anchorName="home.allApps" onPress={onAllApps} />
        </View>
      </View>
      {menuFor != null && <Pressable style={StyleSheet.absoluteFill} onPress={close} />}
    </>
  );
}

function SplitPanes({ split, screen, t }) {
  const p = splitPanes(split, screen);
  const pane = { position: 'absolute', borderRadius: radius.m, backgroundColor: t.chrome };
  return (
    <View pointerEvents="none" style={StyleSheet.absoluteFill}>
      <View style={[{ position: 'absolute', backgroundColor: t.wallTop }, p.seam]} />
      <View style={[pane, p.left]} />
      <View style={[pane, p.right]} />
    </View>
  );
}

// A snapped window that draws bigger than its pane (Clock again) is cut at
// the pane's edges: `pane` clips (a plain rectangle: a rounded clip over a
// whole window would cost the Tab A7's GPU every frame), the inner view puts
// the window back in screen coordinates. The same two views without a pane (the whole screen,
// not clipping): a different tree would mount the Window afresh.
function PaneClip({ pane, screen, children }) {
  const p = pane || { left: 0, top: 0, width: screen.width, height: screen.height };
  return (
    <View pointerEvents="box-none" style={{ position: 'absolute', left: p.left, top: p.top, width: p.width, height: p.height, overflow: pane ? 'hidden' : 'visible' }}>
      <View pointerEvents="box-none" style={{ position: 'absolute', left: -p.left, top: -p.top, width: screen.width, height: screen.height }}>
        {children}
      </View>
    </View>
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
  // Stacked (portrait), the seam is a y between the status bar and `bottom`.
  const vertical = stacked({ left, right });
  const lo = vertical ? screen.topBar : 0;
  const span = vertical ? screen.height - bottom - lo : screen.width;
  const dragX = useNativeDragX();
  const dragY = useNativeDragY();
  const drag = vertical ? dragY : dragX;
  const seam = seamOf({ left, right }, screen);
  const base = grabbed != null ? grabbed : seam;
  const anchor = useAnchor('split.divider');
  const shift = useMemo(
    () => (vertical ? drag.dy : drag.dx).interpolate({
      inputRange: [lo + span * 0.25 - base, lo + span * 0.75 - base],
      outputRange: [lo + span * 0.25 - base, lo + span * 0.75 - base],
      extrapolate: 'clamp',
    }),
    [drag, vertical, base, lo, span],
  );
  useEffect(() => {
    if (grabbed == null) setSettling(null);
  }, [seam, grabbed]);
  const live = useRef({ at: 0, sent: null, timer: null, last: null });
  useEffect(() => () => clearTimeout(live.current.timer), []);
  // The responder must survive the live resizes (seam changes mid-drag), so
  // it reads the current seam and range through a ref.
  const cur = useRef(null);
  cur.current = { seam, lo, span, vertical };
  const pan = useMemo(() => {
    let start = 0;
    let r = { lo: 0, span: 1, vertical: false };
    const along = (g) => (r.vertical ? g.dy : g.dx);
    const clampX = (g) => Math.max(r.lo + r.span * 0.25, Math.min(r.lo + r.span * 0.75, start + along(g)));
    const send = (nx) => {
      const L = live.current;
      L.at = Date.now();
      if (L.sent != null && Math.abs(nx - L.sent) < 1) return;
      L.sent = nx;
      wm.splitRatio((nx - r.lo) / r.span);
    };
    const end = (g) => {
      const L = live.current;
      clearTimeout(L.timer);
      L.timer = null;
      const nx = clampX(g);
      setSettling(nx);
      setGrabbed(null);
      drag.on.setValue(0);
      if (Math.abs(along(g)) > 1) send(nx);
    };
    return PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: () => {
        r = cur.current;
        start = r.seam;
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
  const box = vertical
    ? { left: 0, top: cx - 16, width: screen.width, height: 32, transform: [{ translateY: held ? shift : 0 }] }
    : { left: cx - 16, top: screen.topBar, width: 32, height: screen.height - screen.topBar - bottom, transform: [{ translateX: held ? shift : 0 }] };
  const long = x != null ? 64 : 44;
  const thin = x != null ? 8 : 6;
  return (
    <Animated.View {...pan.panHandlers} {...drag.handlers} ref={anchor.ref} onLayout={anchor.onLayout}
      style={[{ position: 'absolute', alignItems: 'center', justifyContent: 'center' }, box]}>
      <View style={{ width: vertical ? long : thin, height: vertical ? thin : long, borderRadius: 4, backgroundColor: x != null ? t.accent : t.name === 'dark' ? 'rgba(255,255,255,0.7)' : 'rgba(20,24,60,0.5)' }} />
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
  // A turn of the screen moves views whose own layout didn't change (the
  // centred dock): measure every anchor again for test drivers.
  useEffect(() => {
    const id = setTimeout(remeasureAnchors, 600);
    return () => clearTimeout(id);
  }, [dims.width, dims.height]);
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
  // The launcher and the switcher sit over home's wallpaper (one element: the
  // launcher is memoized and stays mounted).
  const wall = useMemo(() => <Wallpaper width={dims.width} height={dims.height} t={t} />, [dims.width, dims.height, t]);

  const [panel, setPanel] = useState(null); // launcher | overview | quick | calendar
  // A panel that was just closed stays mounted while it animates out.
  const [panels, setPanels] = useState({ cur: null, closing: null });
  if (panels.cur !== panel) setPanels({ cur: panel, closing: panels.cur || (panels.closing !== panel ? panels.closing : null) });
  const closingPanel = panels.closing !== panel ? panels.closing : null;
  const panelClosed = useCallback(() => setPanels((p) => ({ ...p, closing: null })), []);
  const [winMenu, setWinMenu] = useState(null);
  const [peek, setPeek] = useState(false);
  const [locked, setLocked] = useState(false);
  // The display going dark (power key, idle timeout; jsui-wl wm:screen)
  // locks the shell, so it wakes to the lock screen.
  useHostMessage('wm:screen', (msg) => {
    if (msg.on) return;
    setPanel(null);
    setLocked(true);
  });
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
  const [notes, setNotes] = useState([]);
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
  // Home on screen: its glance shows the newest notification, so one coming
  // in there only chimes (a toast said the same thing over it).
  const homeRef = useRef(false);
  homeRef.current = !visible.length && !panel && !locked && !launching.length;
  const appMode = settings.tabletMode && (launching.length > 0 || visible.some((w) => w.type === 'normal' && (w.maximized || w.snap)));
  const dockShown = !appMode || peek || panel === 'launcher' || panel === 'overview';
  const bottom = appMode ? scr.homeBar || 24 : scr.dock;
  // Solid over a full-screen app, and over the loading card that stands in for one.
  const solidBar = visible.some((w) => w.maximized || w.snap) || (appMode && launching.length > 0);
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
      appName: app ? cleanName(app.name) : msg.app || '',
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
    const full = { id, time: nowText(), at: Date.now(), notifyId: msg.id, ...note };
    setNotes((l) => [full, ...l].slice(0, 30));
    if (lockedRef.current || (settings.doNotDisturb && msg.urgency < 2)) return;
    if (homeRef.current) playNotify();
    else setToasts((l) => [...l, full].slice(-3));
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
      if (!oskOn && appMode) r.push([W * 0.3, H - bottom, W * 0.4, bottom]); // HomeBar (the app runs under the rest)
    }
    if (edges) {
      r.push([0, scr.topBar, EDGE, H - scr.topBar - bottom]);
      r.push([W - EDGE, scr.topBar, EDGE, H - scr.topBar - bottom]);
    }
    if (split) {
      const cx = seamOf(split, scr);
      r.push(stacked(split) ? [0, cx - 16, W, 32] : [cx - 16, scr.topBar, 32, H - scr.topBar - bottom]);
    }
    if (oskOn && oskRect) r.push([oskRect.x, oskRect.y, oskRect.width, oskRect.height]);
    // Toasts take a tap (it puts them away), not the app under them.
    if (toasts.length) r.push([(W - 400) / 2, scr.topBar + 10, 400, toasts.length * 92]);
    return r;
  }, [toasts.length, locked, panel, dockMenu, winMenu, fullscreen, oskOn, dockRect, oskRect, scr, dockShown, appMode, bottom, edges, split && split.left.x + split.left.w, split && split.right.x, split && split.left.y + split.left.h, split && split.right.y]);
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
  // `from`: the anchor of the icon it was tapped on (the launcher's by default).
  const launch = useCallback((app, from) => {
    wm.haptic();
    startLaunch(app, from || `app.${app.id}`, winsRef.current.some((w) => w.appId === app.id && w.type === 'normal'));
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
  // The status bar names a full-screen app (tablet mode: it has no header of
  // its own) and holds its menu; while an app starts, the one starting.
  const starting = appMode && launching.length ? launching[launching.length - 1].app : null;
  const fullApp = settings.tabletMode && !split && topNormal && topNormal.maximized && !topNormal.fullscreen ? topNormal : null;
  const barApp = starting ? { app: starting, name: cleanName(starting.name) }
    : fullApp ? { app: apps.find((a) => a.id === fullApp.appId), win: fullApp, name: appName(fullApp, apps) } : null;
  const panes = split && !overviewUp ? splitPanes(split, scr) : null;
  const panelEl = (name, open) => {
    const common = { key: name, screen: scr, open, onClose: closePanel, onClosed: open ? undefined : panelClosed };
    if (name === 'overview') return <Overview {...common} windows={visible} active={active} apps={apps} wallpaper={wall} />;
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
        {!visible.length && <Home screen={scr} t={t} note={notes.find((n) => n.notifyId != null && Date.now() - n.at < 3600e3)} apps={apps} settings={settings} onLaunch={launch} onCalendar={() => setPanel('calendar')} onAllApps={() => setPanel('launcher')} />}
        {split && !overviewUp && <SplitPanes split={split} screen={scr} t={t} />}
        {[...windows, ...closingNow.filter((c) => !map[c.id])].map((w) =>
          w.type === 'dock' || w.type === 'desktop' ? null : (
            <View key={w.id} pointerEvents="box-none" style={[StyleSheet.absoluteFill, { opacity: overviewUp ? 0 : 1 }]}>
            <PaneClip screen={scr} pane={panes && map[w.id] && (w.id === split.left.id ? panes.left : w.id === split.right.id ? panes.right : null)}>
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
              splashed={launching.some((l) => l.app.id === w.appId)}
            />
            </PaneClip>
            </View>
          ),
        )}
        {split && !overviewUp && <SplitDivider left={split.left} right={split.right} screen={scr} t={t} bottom={bottom} />}
        <SnapPreview preview={preview} screen={scr} t={t} bottom={bottom} />
        {edges && <EdgeSwipe side="left" top={scr.topBar} bottom={bottom} onSwipe={() => setPanel('launcher')} />}
        {edges && <EdgeSwipe side="right" top={scr.topBar} bottom={bottom} onSwipe={() => setPanel('overview')} />}
        {!fullscreen && (
          <TopBar screen={scr} solid={solidBar} panel={panel} setPanel={setPanel} settings={settings} system={system}
            app={barApp} menuOpen={!!barApp && !!barApp.win && winMenu === barApp.win.id} onMenu={onWinMenu} onMenuClose={closeWinMenu}
            quiet={!visible.length && (!panel || panel === 'quick')} />
        )}
        {!fullscreen && !oskOn && appMode && !dockShown && (
          <HomeBar screen={scr} windows={windows} setPanel={setPanel} onPeek={onPeek} />
        )}
        {/* Home has its own shortcuts and "All apps": no dock while it is all there is. */}
        {!fullscreen && !oskOn && windows.some((w) => w.type === 'normal') && (
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
        <Launcher screen={scr} apps={apps} settings={settings} onLaunch={launch} open={panel === 'launcher'} warm={warmLauncher} wallpaper={wall}
          onClose={closePanel} onClosed={panelClosed} />
        {[closingPanel, panel].filter((n) => n && n !== 'launcher').map((name) => panelEl(name, name === panel))}
        {/* Over the launcher: the card grows out of the tapped icon as the
            launcher fades. Hidden while the launcher or the switcher is up. */}
        {launching.length > 0 && (
          <View pointerEvents="none" style={[StyleSheet.absoluteFill, { opacity: overviewUp || panel === 'launcher' ? 0 : 1 }]}>
            {launching.map((l) => (
              <LaunchSplash key={l.key} launch={l} screen={scr} t={t} done={!!launchDone[l.key]} onGone={launchGone} />
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
  // Home: one centred column, the glance over the shortcut card (a panel's
  // radius).
  home: { position: 'absolute', left: 0, right: 0, alignItems: 'center' },
  glance: { alignItems: 'center' },
  // Smaller and lighter than the lock screen's 132 px bold clock: home and
  // lock don't look alike.
  glanceTime: { fontFamily: font, fontSize: 76, fontWeight: '400', letterSpacing: -2.5, fontVariant: ['tabular-nums'] },
  glanceDate: { fontFamily: font, fontSize: 20, fontWeight: '500', marginTop: 8 },
  glanceLine: { fontFamily: font, fontSize: 14, fontWeight: '400', marginTop: 8, textAlign: 'center', alignSelf: 'stretch' },
  homeCard: { flexDirection: 'row', marginTop: 56, paddingTop: HOME_PAD.top, paddingBottom: HOME_PAD.bottom, paddingHorizontal: HOME_PAD.side, borderRadius: radius.l, borderWidth: 1, borderColor: 'rgba(255,255,255,0.12)', backgroundColor: 'rgba(255,255,255,0.09)' },
  shortcut: { width: HOME_SLOT.w, height: HOME_SLOT.h, alignItems: 'center', paddingTop: 4 },
  shortcutLabel: { fontFamily: font, fontSize: 13, fontWeight: '500', marginTop: 8 },
  allApps: { width: 56, height: 56, borderRadius: 28, alignItems: 'center', justifyContent: 'center' },
  snap: { position: 'absolute', borderRadius: radius.m, borderWidth: 3 },
  // The same shapes as Quick Settings' sliders (controls.jsx track / fill).
  osd: { position: 'absolute', width: 400, height: 48, borderRadius: 24, overflow: 'hidden' },
  osdFill: { position: 'absolute', left: 0, top: 0, bottom: 0, borderRadius: 24 },
  osdIcon: { position: 'absolute', left: 14, top: 13 },
  // Square like the full-screen window it stands in for.
  splash: { position: 'absolute', left: 0, overflow: 'hidden' },
  splashBody: { flex: 1, alignItems: 'center', justifyContent: 'center' },
  spinner: { width: 34, height: 34, borderRadius: 17, borderWidth: 3.5 },
});

AppRegistry.registerComponent('main', () => Shell);
