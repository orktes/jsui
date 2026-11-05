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
import { AppRegistry, DevSettings, PanResponder, StyleSheet, Text, View, useHostMessage, useWindowDimensions, HostTexture } from 'react-native';
import Svg, { Defs, Ellipse, RadialGradient, Stop } from 'react-native-svg';
import { Dock, DockMenu, TopBar, dateText, timeText, useClock } from './bars.jsx';
import { useAnchor } from './anchors.js';
import { Keyboard } from './keyboard.jsx';
import { CalendarPanel, Launcher, LockScreen, Overview, QuickSettings, Toasts } from './panels.jsx';
import { ThemeContext, dark, font, light } from './theme.js';
import { ClosingWindow, Window } from './Window.jsx';
import { appTitle, useApps, usePopups, useScreen, useSettings, useSystem, useWindows, wm } from './wm.js';

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
function DesktopClock({ screen, t, empty }) {
  const now = useClock();
  return (
    <View pointerEvents="none" style={[styles.deskClock, { top: screen.topBar + 70 }]}>
      <Text style={[styles.deskTime, { color: t.name === 'dark' ? 'rgba(255,255,255,0.92)' : 'rgba(20,24,60,0.85)' }]}>{timeText(now)}</Text>
      <Text style={[styles.deskDate, { color: t.name === 'dark' ? 'rgba(255,255,255,0.7)' : 'rgba(20,24,60,0.6)' }]}>{dateText(now)}</Text>
      {empty && (
        <Text style={[styles.deskHint, { color: t.name === 'dark' ? 'rgba(255,255,255,0.55)' : 'rgba(20,24,60,0.5)' }]}>
          Tap Apps to start something · swipe up on the dock for your windows
        </Text>
      )}
    </View>
  );
}

// Where a dragged window will land.
function SnapPreview({ preview, screen, t }) {
  if (!preview) return null;
  const top = screen.topBar + 6;
  const h = screen.height - screen.topBar - screen.dock - 12;
  const half = (screen.width - 18) / 2;
  const r =
    preview === 'max' ? { left: 6, top, width: screen.width - 12, height: h }
      : preview === 'left' ? { left: 6, top, width: half, height: h }
        : { left: 12 + half, top, width: half, height: h };
  return <View pointerEvents="none" style={[styles.snap, r, { borderColor: t.accent, backgroundColor: t.name === 'dark' ? 'rgba(124,140,255,0.18)' : 'rgba(75,92,255,0.14)' }]} />;
}

// Split view: the seam between a left- and a right-snapped window. Drag it to
// share the screen differently (25-75 %).
function SplitDivider({ left, right, screen, t }) {
  const [x, setX] = useState(null);
  const anchor = useAnchor('split.divider');
  const pan = useMemo(() => {
    let x0 = 0;
    return PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: (e) => {
        x0 = e.nativeEvent.pageX;
        setX(x0);
      },
      onPanResponderMove: (e, g) => {
        const nx = Math.max(screen.width * 0.25, Math.min(screen.width * 0.75, x0 + g.dx));
        setX(nx);
        wm.splitRatio(nx / screen.width);
      },
      onPanResponderRelease: () => setX(null),
      onPanResponderTerminate: () => setX(null),
    });
  }, [screen.width]);
  const cx = x != null ? x : (left.x + left.w + right.x) / 2;
  const top = screen.topBar;
  const h = screen.height - screen.topBar - screen.dock;
  return (
    <View {...pan.panHandlers} ref={anchor.ref} onLayout={anchor.onLayout} style={{ position: 'absolute', left: cx - 16, top, width: 32, height: h, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ position: 'absolute', left: 14, top: 0, bottom: 0, width: 4, backgroundColor: x != null ? t.accent : 'rgba(0,0,0,0.55)' }} />
      <View style={{ width: 12, height: 72, borderRadius: 6, backgroundColor: x != null ? t.accent : t.text, boxShadow: '0px 2px 10px rgba(0,0,0,0.5)' }} />
    </View>
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
  const [locked, setLocked] = useState(false);
  const [preview, setPreview] = useState(null);
  const [dragging, setDragging] = useState(null);
  const [dockMenu, setDockMenu] = useState(null);
  const [dockRect, setDockRect] = useState(null);
  const [oskRect, setOskRect] = useState(null);
  const [notes, setNotes] = useState(() => [
    { id: noteId++, icon: 'tablet', title: 'Welcome to the JSUI desktop', body: 'Drag title bars to move, drag to the edges to snap, swipe up on the dock for all windows.', time: nowText() },
  ]);
  const [toasts, setToasts] = useState([]);

  const notify = useCallback((n) => {
    const note = { id: noteId++, time: nowText(), ...n };
    setNotes((l) => [note, ...l].slice(0, 30));
    if (!settings.doNotDisturb) {
      setToasts((l) => [...l, note].slice(-3));
      setTimeout(() => setToasts((l) => l.filter((x) => x.id !== note.id)), 3200);
    }
  }, [settings.doNotDisturb]);

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
  const split = splitL && splitR && !fullscreen ? { left: splitL, right: splitR } : null;

  // Hotkeys from the host: Super toggles the launcher, Alt+Tab the overview.
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
    if (locked || panel || dockMenu) return [[0, 0, W, H]];
    const r = [];
    if (!fullscreen) {
      r.push([0, 0, W, scr.topBar]);
      if (!settings.osk && dockRect) r.push([dockRect.x, H - scr.dock + dockRect.y, dockRect.width, dockRect.height]);
      else if (!settings.osk) r.push([0, H - scr.dock, W, scr.dock]);
    }
    if (split) {
      const cx = (split.left.x + split.left.w + split.right.x) / 2;
      r.push([cx - 16, scr.topBar, 32, H - scr.topBar - scr.dock]);
    }
    if (settings.osk && oskRect) r.push([oskRect.x, oskRect.y, oskRect.width, oskRect.height]);
    return r;
  }, [locked, panel, dockMenu, fullscreen, settings.osk, dockRect, oskRect, scr, split && split.left.x + split.left.w, split && split.right.x]);
  const lastRegions = useRef('');
  useEffect(() => {
    const json = JSON.stringify(regions);
    if (json !== lastRegions.current) {
      lastRegions.current = json;
      wm.shellRegions(regions);
    }
  }, [regions]);

  const launch = useCallback((app) => {
    wm.launch(app.exec);
    notify({ icon: 'app', title: `Opening ${app.name}`, body: app.comment || '' });
  }, [notify]);

  const title = activeWin ? activeWin.title || appTitle(activeWin, apps) : 'Desktop';
  const closePanel = useCallback(() => setPanel(null), []);
  const onDragging = useCallback((id, on) => setDragging(on ? id : null), []);

  return (
    <ThemeContext.Provider value={t}>
      <View style={styles.root}>
        <Wallpaper width={scr.width} height={scr.height} t={t} />
        {!visible.length && <DesktopClock screen={scr} t={t} empty />}
        {windows.map((w) =>
          w.type === 'dock' || w.type === 'desktop' ? null : (
            <View key={w.id} pointerEvents="box-none" style={[StyleSheet.absoluteFill, { opacity: panel === 'overview' ? 0 : 1 }]}>
            <Window
              win={w}
              active={w.id === active}
              screen={scr}
              apps={apps}
              hidden={panel === 'overview' || !w.ready}
              onPreview={setPreview}
              onDragging={onDragging}
            />
            </View>
          ),
        )}
        {closing.map((w) => (
          <ClosingWindow key={`c${w.id}`} win={w} screen={scr} onDone={doneClosing} />
        ))}
        {split && panel !== 'overview' && <SplitDivider left={split.left} right={split.right} screen={scr} t={t} />}
        <SnapPreview preview={preview} screen={scr} t={t} />
        {!fullscreen && (
          <TopBar screen={scr} title={title} panel={panel} setPanel={setPanel} settings={settings} system={system} />
        )}
        {!fullscreen && !settings.osk && (
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
          />
        )}
        {panel === 'overview' && <Overview screen={scr} windows={visible} active={active} apps={apps} onClose={closePanel} />}
        {panel === 'launcher' && <Launcher screen={scr} apps={apps} settings={settings} onClose={closePanel} onLaunch={launch} />}
        {panel === 'quick' && (
          <QuickSettings
            screen={scr}
            settings={settings}
            system={system}
            windows={windows}
            onClose={closePanel}
            onLock={() => {
              setPanel(null);
              setLocked(true);
            }}
            onReload={() => DevSettings.reload()}
          />
        )}
        {panel === 'calendar' && <CalendarPanel screen={scr} notifications={notes} onClear={() => setNotes([])} onClose={closePanel} />}
        {dockMenu && (
          <DockMenu item={dockMenu} anchor={dockRect && { x: dockRect.x + dockRect.width / 2, width: 0 }} onClose={() => setDockMenu(null)} />
        )}
        {settings.osk && <Keyboard screen={scr} onMeasure={setOskRect} />}
        {locked && <LockScreen screen={scr} notifications={notes} onUnlock={() => setLocked(false)} />}
        <Toasts screen={scr} toasts={toasts} />
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
  deskTime: { fontFamily: font, fontSize: 112, fontWeight: '700', letterSpacing: -3 },
  deskDate: { fontFamily: font, fontSize: 24, fontWeight: '500', marginTop: -4 },
  deskHint: { fontFamily: font, fontSize: 16, marginTop: 22 },
  snap: { position: 'absolute', borderRadius: 18, borderWidth: 3 },
});

AppRegistry.registerComponent('main', () => Shell);
