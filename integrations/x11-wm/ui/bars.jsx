// The always-there parts of the shell: the top bar (apps button, active app,
// clock, status icons) and the floating dock (pinned and running apps).
import React, { memo, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, LayoutAnimation, PanResponder, Pressable, StyleSheet, Text, View } from 'react-native';
import { useAnchor } from './anchors.js';
import { AppIcon } from './AppIcon.jsx';
import { Icon } from './icons.jsx';
import { font, useTheme } from './theme.js';
import { appKeyOf, appTitle, setSetting, wm } from './wm.js';

export function useClock() {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    let id;
    const tick = () => {
      setNow(new Date());
      id = setTimeout(tick, 60000 - (Date.now() % 60000) + 20);
    };
    id = setTimeout(tick, 60000 - (Date.now() % 60000) + 20);
    return () => clearTimeout(id);
  }, []);
  return now;
}

const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
export const pad2 = (n) => String(n).padStart(2, '0');
export const timeText = (d) => `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
export const dateText = (d) => `${DAYS[d.getDay()]} ${d.getDate()} ${MONTHS[d.getMonth()]}`;
export const longMonth = (d) =>
  ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'][d.getMonth()];

// Swipe down on the bar opens a panel (left half: calendar, right half: quick settings).
function useSwipeDown(onSwipe) {
  const ref = useRef(null);
  ref.current = onSwipe;
  return useMemo(
    () =>
      PanResponder.create({
        onMoveShouldSetPanResponder: (e, g) => g.dy > 12 && Math.abs(g.dy) > Math.abs(g.dx),
        onPanResponderRelease: (e, g) => {
          if (g.dy > 30) ref.current(g.x0);
        },
      }),
    [],
  );
}

export const TopBar = memo(function TopBar({ screen, solid, panel, setPanel, settings, system }) {
  const t = useTheme();
  const now = useClock();
  const apps = useAnchor('topbar.apps');
  const clock = useAnchor('topbar.clock');
  const status = useAnchor('topbar.status');
  const kbd = useAnchor('topbar.keyboard');
  const swipe = useSwipeDown((x) => setPanel(x < screen.width / 2 ? 'calendar' : 'quick'));
  const toggle = (p) => setPanel(panel === p ? null : p);
  const battery = system.battery;
  return (
    <View {...swipe.panHandlers} style={[styles.bar, { height: screen.topBar, backgroundColor: solid ? t.bar : 'transparent', borderColor: solid ? t.panelLine : 'transparent' }]}>
      <View style={styles.side}>
        <Pressable ref={apps.ref} onLayout={apps.onLayout} onPress={() => toggle('launcher')}
          style={({ pressed }) => [styles.barButton, (pressed || panel === 'launcher') && { backgroundColor: t.panelLine }]}>
          <Icon name="grid" size={20} color={t.text} />
          <Text style={[styles.barText, { color: t.text, marginLeft: 8 }]}>Apps</Text>
        </Pressable>
      </View>
      <Pressable ref={clock.ref} onLayout={clock.onLayout} onPress={() => toggle('calendar')}
        style={({ pressed }) => [styles.clock, (pressed || panel === 'calendar') && { backgroundColor: t.panelLine }]}>
        <Text style={[styles.barText, styles.tabular, { color: t.textDim, fontWeight: '500' }]}>{dateText(now)}</Text>
        <Text style={[styles.barText, styles.tabular, { color: t.text, marginLeft: 10 }]}>{timeText(now)}</Text>
        {settings.doNotDisturb && <View style={{ marginLeft: 8 }}><Icon name="moon" size={16} color={t.textDim} /></View>}
      </Pressable>
      <View style={[styles.side, { justifyContent: 'flex-end' }]}>
        <Pressable ref={kbd.ref} onLayout={kbd.onLayout} onPress={() => setSetting('osk', !settings.osk)}
          style={({ pressed }) => [styles.barButton, (pressed || settings.osk) && { backgroundColor: t.panelLine }]}>
          <Icon name="keyboard" size={22} color={t.text} />
        </Pressable>
        <Pressable ref={status.ref} onLayout={status.onLayout} onPress={() => toggle('quick')}
          style={({ pressed }) => [styles.barButton, styles.status, (pressed || panel === 'quick') && { backgroundColor: t.panelLine }]}>
          <Icon name="wifi" size={20} color={settings.wifi ? t.text : t.textMute} />
          <View style={{ width: 12 }} />
          <Icon name={settings.muted || settings.volume === 0 ? 'mute' : 'volume'} size={20} color={t.text} />
          <View style={{ width: 12 }} />
          {battery ? (
            <>
              <Icon name="battery" size={22} color={t.text} />
              <Text style={[styles.barText, styles.tabular, { color: t.text, marginLeft: 6, fontWeight: '500' }]}>{battery.percent}%</Text>
            </>
          ) : (
            <Icon name="plug" size={20} color={t.text} />
          )}
        </Pressable>
      </View>
    </View>
  );
});

// ------------------------------------------------------------------- Dock

// When the dock last saw a swipe: a press that turned into a swipe must not
// also fire its long-press menu.
let lastDockSwipe = 0;

const DockItem = memo(function DockItem({ item, apps, active, onLongPress, size }) {
  const t = useTheme();
  const anchor = useAnchor(`dock.${item.key}`);
  const bounce = useRef(new Animated.Value(0)).current;
  const onPress = () => {
    const ws = item.windows;
    if (!ws.length) {
      if (item.app) {
        Animated.sequence([
          Animated.timing(bounce, { toValue: 1, duration: 150, easing: Easing.out(Easing.quad), useNativeDriver: true }),
          Animated.spring(bounce, { toValue: 0, friction: 4, tension: 120, useNativeDriver: true }),
        ]).start();
        wm.launch(item.app.exec);
      }
      return;
    }
    const focused = ws.find((w) => w.id === active);
    if (focused && ws.length === 1) wm.minimize(focused.id);
    else if (focused) {
      // Cycle through this app's windows.
      const i = ws.indexOf(focused);
      wm.focus(ws[(i + 1) % ws.length].id);
    } else {
      const top = ws[ws.length - 1];
      wm.focus(top.id);
    }
  };
  const ty = bounce.interpolate({ inputRange: [0, 1], outputRange: [0, -14] });
  const running = item.windows.length;
  const isActive = item.windows.some((w) => w.id === active);
  return (
    <Pressable
      ref={anchor.ref}
      onLayout={anchor.onLayout}
      onPress={onPress}
      onLongPress={() => Date.now() - lastDockSwipe > 800 && onLongPress(item)}
      delayLongPress={450}
      style={({ pressed }) => [styles.dockItem, { width: size + 12 }, pressed && { transform: [{ scale: 0.92 }] }]}
    >
      <Animated.View style={{ transform: [{ translateY: ty }] }}>
        <AppIcon app={item.app} win={item.windows[0]} size={size} />
      </Animated.View>
      <View style={styles.dots}>
        {running > 0 && (
          <View style={[styles.dot, { backgroundColor: isActive ? t.accent : t.textDim, width: isActive ? 16 : 5 }]} />
        )}
      </View>
    </Pressable>
  );
});

export function dockItems(apps, windows, pinned) {
  const items = [];
  const byKey = new Map();
  for (const id of pinned) {
    const app = apps.find((a) => a.id === id);
    if (!app) continue;
    const it = { key: id, app, windows: [], pinned: true };
    items.push(it);
    byKey.set(id, it);
  }
  for (const w of windows) {
    if (w.type === 'dialog' || w.type === 'splash' || w.transientFor) continue;
    const key = appKeyOf(w);
    let it = byKey.get(key);
    if (!it) {
      it = { key, app: apps.find((a) => a.id === w.appId), windows: [], pinned: false };
      items.push(it);
      byKey.set(key, it);
    }
    it.windows.push(w);
  }
  return items;
}

export const Dock = memo(function Dock({ screen, apps, windows, active, settings, panel, setPanel, onMenu, onMeasure, shown = true }) {
  const t = useTheme();
  const items = useMemo(() => dockItems(apps, windows, settings.pinned), [apps, windows, settings.pinned]);
  const count = useRef(items.length);
  if (count.current !== items.length) {
    count.current = items.length;
    LayoutAnimation.configureNext(LayoutAnimation.create(200, 'easeInEaseOut', 'opacity'));
  }
  const launcher = useAnchor('dock.launcher');
  const overview = useAnchor('dock.overview');
  // Gestures on the dock: swipe up opens the switcher; swipe sideways jumps
  // straight to the previous app (left) or cycles to the oldest one (right).
  const recent = useRef([]);
  recent.current = windows.filter((w) => !w.minimized && w.type === 'normal' && !w.transientFor);
  const swipe = useMemo(
    () =>
      PanResponder.create({
        onMoveShouldSetPanResponderCapture: (e, g) =>
          (g.dy < -14 && Math.abs(g.dy) > Math.abs(g.dx)) || (Math.abs(g.dx) > 24 && Math.abs(g.dx) > Math.abs(g.dy) * 1.5),
        onPanResponderTerminationRequest: () => false,
        onPanResponderGrant: () => {
          lastDockSwipe = Date.now();
          onMenu(null); // a slow swipe may have opened an item's long-press menu
        },
        onPanResponderRelease: (e, g) => {
          lastDockSwipe = Date.now();
          if (g.dy < -40 && Math.abs(g.dy) > Math.abs(g.dx)) return setPanel('overview');
          const ws = recent.current; // bottom -> top: the top is the current app
          if (ws.length < 2 || Math.abs(g.dx) < 70) return;
          wm.focus(g.dx < 0 ? ws[ws.length - 2].id : ws[0].id);
        },
      }),
    [setPanel, onMenu],
  );
  // In tablet mode over a full-screen app the dock hides below the edge and
  // slides up when the home indicator is swiped or tapped.
  const slide = useRef(new Animated.Value(shown ? 0 : 1)).current;
  useEffect(() => {
    Animated.spring(slide, { toValue: shown ? 0 : 1, tension: 170, friction: 22, useNativeDriver: true }).start();
  }, [shown, slide]);
  const dy = slide.interpolate({ inputRange: [0, 1], outputRange: [0, screen.dock + 24] });
  const size = 52;
  const h = screen.dock - 8;
  const plate = (on) => ({ width: size, height: size, borderRadius: size * 0.28, backgroundColor: on ? t.accent : t.name === 'dark' ? 'rgba(255,255,255,0.12)' : 'rgba(20,24,60,0.08)' });
  return (
    <Animated.View pointerEvents={shown ? 'box-none' : 'none'} style={[styles.dockWrap, { height: screen.dock, transform: [{ translateY: dy }] }]}>
      <View
        {...swipe.panHandlers}
        onLayout={(e) => onMeasure(e.nativeEvent.layout)}
        style={[styles.dock, { height: h, borderRadius: h / 2.4, backgroundColor: t.bar, borderColor: t.panelLine, boxShadow: `0px 12px 40px ${t.shadow}` }]}
      >
        <Pressable ref={launcher.ref} onLayout={launcher.onLayout} onPress={() => setPanel(panel === 'launcher' ? null : 'launcher')}
          style={({ pressed }) => [styles.dockItem, { width: size + 12 }, pressed && { transform: [{ scale: 0.92 }] }]}>
          <View style={[styles.launcherBtn, plate(panel === 'launcher')]}>
            <Icon name="grid" size={26} color={panel === 'launcher' ? t.accentText : t.text} />
          </View>
          <View style={styles.dots} />
        </Pressable>
        <View style={[styles.sep, { backgroundColor: t.panelLine }]} />
        {items.map((it) => (
          <DockItem key={it.key} item={it} apps={apps} active={active} size={size} onLongPress={onMenu} />
        ))}
        <View style={[styles.sep, { backgroundColor: t.panelLine }]} />
        <Pressable ref={overview.ref} onLayout={overview.onLayout} onPress={() => setPanel(panel === 'overview' ? null : 'overview')}
          style={({ pressed }) => [styles.dockItem, { width: size + 12 }, pressed && { transform: [{ scale: 0.92 }] }]}>
          <View style={[styles.launcherBtn, plate(panel === 'overview')]}>
            <Icon name="overview" size={26} color={panel === 'overview' ? t.accentText : t.text} />
          </View>
          <View style={styles.dots} />
        </Pressable>
      </View>
    </Animated.View>
  );
});

// Tablet mode over a full-screen app: the home indicator along the bottom
// edge. Swipe up a little (or tap) for the dock, further for the switcher,
// sideways to flip between the last apps.
export const HomeBar = memo(function HomeBar({ screen, windows, setPanel, onPeek }) {
  const t = useTheme();
  const anchor = useAnchor('homebar');
  const lift = useRef(new Animated.Value(0)).current;
  const recent = useRef([]);
  recent.current = windows.filter((w) => !w.minimized && w.type === 'normal' && !w.transientFor);
  const pan = useMemo(
    () =>
      PanResponder.create({
        onStartShouldSetPanResponder: () => true,
        onMoveShouldSetPanResponder: () => true,
        onPanResponderTerminationRequest: () => false,
        onPanResponderMove: (e, g) => lift.setValue(Math.max(-40, Math.min(0, g.dy * 0.35))),
        onPanResponderRelease: (e, g) => {
          Animated.spring(lift, { toValue: 0, friction: 6, useNativeDriver: true }).start();
          if (g.dy < -140 || (g.dy < -60 && g.vy < -1.2)) return setPanel('overview');
          if (Math.abs(g.dx) > 70 && Math.abs(g.dx) > Math.abs(g.dy)) {
            const ws = recent.current;
            if (ws.length >= 2) wm.focus(g.dx < 0 ? ws[ws.length - 2].id : ws[0].id);
            return;
          }
          onPeek();
        },
      }),
    [lift, setPanel, onPeek],
  );
  return (
    <View {...pan.panHandlers} ref={anchor.ref} onLayout={anchor.onLayout}
      style={[styles.homeBar, { height: screen.homeBar || 24 }]}>
      <Animated.View style={[styles.homePill, { backgroundColor: t.name === 'dark' ? 'rgba(255,255,255,0.55)' : 'rgba(20,24,60,0.45)', transform: [{ translateY: lift }] }]} />
    </View>
  );
});

// Long-press menu of a dock item.
export const DockMenu = memo(function DockMenu({ item, anchor, onClose }) {
  const t = useTheme();
  if (!item) return null;
  const run = (fn) => () => {
    fn();
    onClose();
  };
  const rows = [];
  if (item.app) rows.push({ icon: 'plus', label: 'New window', fn: () => wm.launch(item.app.exec) });
  for (const w of item.windows.slice(0, 5)) rows.push({ icon: 'app', label: w.title || 'Window', fn: () => wm.focus(w.id) });
  if (item.windows.length) rows.push({ icon: 'close', label: item.windows.length > 1 ? `Close ${item.windows.length} windows` : 'Close window', fn: () => item.windows.forEach((w) => wm.close(w.id)), danger: true });
  const x = anchor ? anchor.x + anchor.width / 2 - 140 : 200;
  return (
    <Pressable style={StyleSheet.absoluteFill} onPress={onClose}>
      <View onStartShouldSetResponder={() => true}
        style={[styles.menu, { left: Math.max(12, x), bottom: 96, backgroundColor: t.panel, borderColor: t.panelLine, boxShadow: `0px 16px 48px ${t.shadow}` }]}>
        <Text numberOfLines={1} style={[styles.menuTitle, { color: t.textDim }]}>{(item.app && item.app.name) || (item.windows[0] && appTitle(item.windows[0], []))}</Text>
        {rows.map((r, i) => (
          <Pressable key={i} onPress={run(r.fn)} style={({ pressed }) => [styles.menuRow, pressed && { backgroundColor: t.panelLine }]}>
            <Icon name={r.icon} size={20} color={r.danger ? t.danger : t.text} />
            <Text numberOfLines={1} style={[styles.menuText, { color: r.danger ? t.danger : t.text }]}>{r.label}</Text>
          </Pressable>
        ))}
      </View>
    </Pressable>
  );
});

const styles = StyleSheet.create({
  bar: { position: 'absolute', left: 0, right: 0, top: 0, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 6, borderBottomWidth: 1 },
  side: { flex: 1, flexDirection: 'row', alignItems: 'center' },
  barButton: { height: 34, minWidth: 44, borderRadius: 17, paddingHorizontal: 12, flexDirection: 'row', alignItems: 'center', justifyContent: 'center' },
  status: { paddingHorizontal: 14 },
  barText: { fontFamily: font, fontSize: 15, fontWeight: '600' },
  tabular: { fontVariant: ['tabular-nums'] },
  homeBar: { position: 'absolute', left: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center' },
  homePill: { width: 136, height: 5, borderRadius: 3 },
  clock: { height: 34, borderRadius: 17, paddingHorizontal: 16, flexDirection: 'row', alignItems: 'center' },
  dockWrap: { position: 'absolute', left: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center' },
  dock: { flexDirection: 'row', alignItems: 'center', paddingHorizontal: 10, borderWidth: 1 },
  dockItem: { height: 72, alignItems: 'center', justifyContent: 'flex-start', paddingTop: 6 },
  launcherBtn: { alignItems: 'center', justifyContent: 'center' },
  dots: { flexDirection: 'row', height: 5, marginTop: 4 },
  dot: { width: 5, height: 5, borderRadius: 2.5 },
  sep: { width: 1, height: 40, marginHorizontal: 6 },
  menu: { position: 'absolute', width: 280, borderRadius: 22, borderWidth: 1, paddingVertical: 8 },
  menuTitle: { fontFamily: font, fontSize: 13, fontWeight: '700', paddingHorizontal: 18, paddingVertical: 8, letterSpacing: 0.4 },
  menuRow: { height: 52, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 18 },
  menuText: { fontFamily: font, fontSize: 16, marginLeft: 14, flex: 1 },
});
