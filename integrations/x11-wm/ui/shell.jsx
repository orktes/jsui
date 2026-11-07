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
import { Animated, AppRegistry, DevSettings, PanResponder, Pressable, StyleSheet, Text, View, useHostMessage, useWindowDimensions, HostTexture } from 'react-native';
import Svg, { Defs, Ellipse, RadialGradient, Stop } from 'react-native-svg';
import { Dock, DockMenu, HomeBar, TopBar, dateText, timeText, useClock } from './bars.jsx';
import { Icon } from './icons.jsx';
import { useAnchor } from './anchors.js';
import { useNativeDragX } from './drag.js';
import { Keyboard } from './keyboard.jsx';
import { CalendarPanel, Launcher, LockScreen, Overview, QuickSettings, Toasts } from './panels.jsx';
import { ThemeContext, dark, font, light } from './theme.js';
import { ClosingWindow, Window } from './Window.jsx';
import { appName, useApps, usePopups, useScreen, useSettings, useSystem, useWindows, wm } from './wm.js';

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

// Big clock on the empty desktop.
function DesktopClock({ screen, t }) {
  const now = useClock();
  return (
    <View pointerEvents="none" style={[styles.deskClock, { top: screen.topBar + 70 }]}>
      <Text style={[styles.deskTime, { color: t.name === 'dark' ? 'rgba(255,255,255,0.92)' : 'rgba(20,24,60,0.85)' }]}>{timeText(now)}</Text>
      <Text style={[styles.deskDate, { color: t.name === 'dark' ? 'rgba(255,255,255,0.7)' : 'rgba(20,24,60,0.6)' }]}>{dateText(now)}</Text>
    </View>
  );
}

// Where a dragged window will land.
function SnapPreview({ preview, screen, t, bottom }) {
  if (!preview) return null;
  const top = screen.topBar + 6;
  const h = screen.height - screen.topBar - bottom - 12;
  const half = (screen.width - 18) / 2;
  const r =
    preview === 'max' ? { left: 6, top, width: screen.width - 12, height: h }
      : preview === 'left' ? { left: 6, top, width: half, height: h }
        : { left: 12 + half, top, width: half, height: h };
  return <View pointerEvents="none" style={[styles.snap, r, { borderColor: t.accent, backgroundColor: t.name === 'dark' ? 'rgba(124,140,255,0.18)' : 'rgba(75,92,255,0.14)' }]} />;
}

// Split view: the seam between a left- and a right-snapped window. Drag it to
// share the screen differently (25-75 %).
function SplitDivider({ left, right, screen, t, bottom }) {
  // The grip follows the finger natively, clamped to 25-75 % of the screen;
  // the two windows are resized once, on release (resizing them on every
  // move had both apps relayout at the rate of the finger).
  const [grabbed, setGrabbed] = useState(false);
  const [settling, setSettling] = useState(null); // where it was let go, until the windows follow
  const drag = useNativeDragX();
  const seam = (left.x + left.w + right.x) / 2;
  const anchor = useAnchor('split.divider');
  const shift = useMemo(
    () => drag.dx.interpolate({
      inputRange: [screen.width * 0.25 - seam, screen.width * 0.75 - seam],
      outputRange: [screen.width * 0.25 - seam, screen.width * 0.75 - seam],
      extrapolate: 'clamp',
    }),
    [drag, seam, screen.width],
  );
  useEffect(() => setSettling(null), [seam]);
  const pan = useMemo(() => {
    const end = (g) => {
      const nx = Math.max(screen.width * 0.25, Math.min(screen.width * 0.75, seam + g.dx));
      setSettling(nx);
      setGrabbed(false);
      drag.on.setValue(0);
      if (Math.abs(g.dx) > 1) wm.splitRatio(nx / screen.width);
    };
    return PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: () => {
        drag.on.setValue(1);
        setGrabbed(true);
      },
      onPanResponderRelease: (e, g) => end(g),
      onPanResponderTerminate: (e, g) => end(g),
    });
  }, [screen.width, seam, drag]);
  const x = grabbed ? seam : settling;
  const cx = x != null ? x : seam;
  const top = screen.topBar;
  const h = screen.height - screen.topBar - bottom;
  return (
    <Animated.View {...pan.panHandlers} {...drag.handlers} ref={anchor.ref} onLayout={anchor.onLayout}
      style={{ position: 'absolute', left: cx - 16, top, width: 32, height: h, alignItems: 'center', justifyContent: 'center', transform: [{ translateX: grabbed ? shift : 0 }] }}>
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

  const notify = useCallback((n) => {
    const note = { id: noteId++, time: nowText(), ...n };
    setNotes((l) => [note, ...l].slice(0, 30));
    if (!settings.doNotDisturb) {
      setToasts((l) => [...l, note].slice(-3));
    }
  }, [settings.doNotDisturb]);
  const toastDone = useCallback((id) => setToasts((l) => l.filter((x) => x.id !== id)), []);
  const dismissNote = useCallback((id) => setNotes((l) => l.filter((x) => x.id !== id)), []);

  // Windows that disappeared animate out from their last state.
  const prev = useRef(new Map());
  const [closing, setClosing] = useState([]);
  useEffect(() => {
    const gone = [];
    for (const [id, w] of prev.current) if (!map[id] && w.ready && !w.minimized) gone.push(w);
    if (gone.length) setClosing((c) => [...c, ...gone]);
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
  // Tablet mode with an app on screen: apps reach the bottom edge, the dock
  // hides behind the home indicator.
  const appMode = settings.tabletMode && visible.some((w) => w.type === 'normal' && (w.maximized || w.snap));
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
  useHostMessage('wm:launchFailed', (msg) => {
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
    if (split) {
      const cx = (split.left.x + split.left.w + split.right.x) / 2;
      r.push([cx - 16, scr.topBar, 32, H - scr.topBar - bottom]);
    }
    if (oskOn && oskRect) r.push([oskRect.x, oskRect.y, oskRect.width, oskRect.height]);
    // Toasts take a tap (it puts them away), not the app under them.
    if (toasts.length) r.push([(W - 400) / 2, scr.topBar + 10, 400, toasts.length * 92]);
    return r;
  }, [toasts.length, locked, panel, dockMenu, winMenu, fullscreen, oskOn, dockRect, oskRect, scr, dockShown, appMode, bottom, split && split.left.x + split.left.w, split && split.right.x]);
  const lastRegions = useRef('');
  useEffect(() => {
    const json = JSON.stringify(regions);
    if (json !== lastRegions.current) {
      lastRegions.current = json;
      wm.shellRegions(regions);
    }
  }, [regions]);

  const launch = useCallback((app) => wm.launch(app.exec), []);

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
    if (name === 'calendar') return <CalendarPanel {...common} notifications={notes} onClear={() => setNotes([])} onDismiss={dismissNote} />;
    return null;
  };

  return (
    <ThemeContext.Provider value={t}>
      <View style={styles.root}>
        <Wallpaper width={scr.width} height={scr.height} t={t} />
        {!visible.length && !panel && <DesktopClock screen={scr} t={t} />}
        {split && !overviewUp && <View pointerEvents="none" style={{ position: 'absolute', left: split.left.x + split.left.w - 1, width: split.right.x - split.left.x - split.left.w + 2, top: split.left.y - scr.titleBar, height: split.left.h + scr.titleBar, backgroundColor: t.name === 'dark' ? '#000' : t.wallTop }} />}
        {windows.map((w) =>
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
            />
            </View>
          ),
        )}
        {closing.map((w) => (
          <ClosingWindow key={`c${w.id}`} win={w} screen={scr} onDone={doneClosing} />
        ))}
        {split && !overviewUp && <SplitDivider left={split.left} right={split.right} screen={scr} t={t} bottom={bottom} />}
        <SnapPreview preview={preview} screen={scr} t={t} bottom={bottom} />
        {!fullscreen && (
          <TopBar screen={scr} solid={solidBar} panel={panel} setPanel={setPanel} settings={settings} system={system} />
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
        <Launcher screen={scr} apps={apps} settings={settings} onLaunch={launch} open={panel === 'launcher'}
          onClose={closePanel} onClosed={panelClosed} />
        {[closingPanel, panel].filter((n) => n && n !== 'launcher').map((name) => panelEl(name, name === panel))}
        {/* The window menu itself is an overlay of its button (WindowMenu.jsx): taps elsewhere close it. */}
        {winMenu != null && <Pressable style={StyleSheet.absoluteFill} onPress={closeWinMenu} />}
        {dockMenu && (
          <DockMenu item={dockMenu} anchor={dockRect && { x: dockRect.x + dockRect.width / 2, width: 0 }} onClose={() => setDockMenu(null)} />
        )}
        {oskOn && <Keyboard screen={scr} onMeasure={setOskRect} onHide={hideOsk} />}
        {locked && (
          <LockScreen screen={scr} notifications={notes} onUnlock={() => setLocked(false)}>
            <Wallpaper width={scr.width} height={scr.height} t={t} />
          </LockScreen>
        )}
        <Toasts screen={scr} toasts={toasts} onDone={toastDone} />
        {Object.values(popups).map((p) =>
          p.ready ? (
            <HostTexture key={p.id} name={`win:${p.id}`} pointerEvents="none"
              style={{ position: 'absolute', left: p.x, top: p.y, width: p.w, height: p.h, boxShadow: '0px 6px 20px rgba(0,0,0,0.35)' }} />
          ) : null,
        )}
        {(settings.brightness < 1 || settings.nightLight) && (
          <View pointerEvents="none" style={[StyleSheet.absoluteFill, { backgroundColor: settings.nightLight ? `rgba(60,30,0,${0.18 + (1 - settings.brightness) * 0.6})` : `rgba(0,0,0,${(1 - settings.brightness) * 0.75})` }]} />
        )}
      </View>
    </ThemeContext.Provider>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, overflow: 'hidden' },
  deskClock: { position: 'absolute', left: 0, right: 0, alignItems: 'center' },
  deskTime: { fontFamily: font, fontSize: 112, fontWeight: '600', letterSpacing: -3, fontVariant: ['tabular-nums'] },
  deskDate: { fontFamily: font, fontSize: 24, fontWeight: '500', marginTop: -4 },
  snap: { position: 'absolute', borderRadius: 18, borderWidth: 3 },
});

AppRegistry.registerComponent('main', () => Shell);
