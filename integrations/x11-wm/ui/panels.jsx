// Full-screen and drop-down panels: the app launcher, the window overview,
// quick settings, the calendar / notifications panel and the lock screen.
import React, { memo, useEffect, useMemo, useRef, useState } from 'react';
import {
  Animated, Easing, HostTexture, PanResponder, Pressable, ScrollView, StyleSheet, Text, TextInput, View,
} from 'react-native';
import { remeasureAnchors, useAnchor } from './anchors.js';
import { AppIcon } from './AppIcon.jsx';
import { dateText, longMonth, pad2, timeText, useClock } from './bars.jsx';
import { Card, IconButton, Label, Slider, ToggleTile } from './controls.jsx';
import { Icon } from './icons.jsx';
import { font, useTheme } from './theme.js';
import { appTitle, setSetting, wm } from './wm.js';

// Animates 0 -> 1 on mount (and back before onClosed when `open` goes false).
function useEntrance(open, duration = 220) {
  const v = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    Animated.timing(v, { toValue: open ? 1 : 0, duration, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, [open, v, duration]);
  return v;
}

// ---------------------------------------------------------------- Launcher

const AppTile = memo(function AppTile({ app, onLaunch }) {
  const t = useTheme();
  const anchor = useAnchor(`app.${app.id}`);
  return (
    <Pressable
      ref={anchor.ref}
      onLayout={anchor.onLayout}
      onPress={() => onLaunch(app)}
      style={({ pressed }) => [styles.appTile, pressed && { backgroundColor: t.panelLine, transform: [{ scale: 0.95 }] }]}
    >
      <AppIcon app={app} size={72} />
      <Text numberOfLines={2} style={[styles.appName, { color: t.text }]}>{app.name}</Text>
    </Pressable>
  );
});

export const Launcher = memo(function Launcher({ screen, apps, onClose, onLaunch, settings }) {
  const t = useTheme();
  const [query, setQuery] = useState('');
  const v = useEntrance(true, 240);
  const input = useRef(null);
  useEffect(() => {
    wm.keyboard(true); // the search field gets the physical keyboard
    const id = setTimeout(() => input.current && input.current.focus && input.current.focus(), 50);
    return () => {
      clearTimeout(id);
      wm.keyboard(false);
    };
  }, []);
  const list = useMemo(() => {
    const q = query.trim().toLowerCase();
    if (!q) return apps;
    return apps.filter((a) => a.name.toLowerCase().includes(q) || (a.comment || '').toLowerCase().includes(q) || a.exec.toLowerCase().includes(q));
  }, [apps, query]);
  const launch = (app) => {
    onLaunch(app);
    onClose();
  };
  const ty = v.interpolate({ inputRange: [0, 1], outputRange: [40, 0] });
  const cols = Math.max(3, Math.floor((screen.width - 160) / 150));
  return (
    <Animated.View style={[StyleSheet.absoluteFill, { opacity: v }]}>
      <Pressable style={[StyleSheet.absoluteFill, { backgroundColor: t.scrim }]} onPress={onClose} />
      <Animated.View pointerEvents="box-none" style={[styles.launcher, { top: screen.topBar + 24, bottom: settings.osk ? 330 : screen.dock + 12, transform: [{ translateY: ty }] }]}>
        <View style={[styles.search, { backgroundColor: t.panel, borderColor: t.panelLine, boxShadow: `0px 12px 36px ${t.shadow}` }]}>
          <Icon name="search" size={24} color={t.textDim} />
          <TextInput
            ref={input}
            value={query}
            onChangeText={setQuery}
            placeholder="Search apps"
            placeholderTextColor={t.textMute}
            onSubmitEditing={() => list[0] && launch(list[0])}
            style={[styles.searchInput, { color: t.text }]}
          />
          {query ? <IconButton icon="close" size={44} iconSize={20} onPress={() => setQuery('')} /> : null}
          <IconButton icon="keyboard" size={44} iconSize={22} active={settings.osk} onPress={() => setSetting('osk', !settings.osk)} />
        </View>
        <Card style={{ flex: 1, marginTop: 20, width: cols * 150 + 40, paddingHorizontal: 20 }}>
          <Label style={{ marginBottom: 8, marginLeft: 6 }}>{query ? `${list.length} RESULT${list.length === 1 ? '' : 'S'}` : 'ALL APPS'}</Label>
          <ScrollView contentContainerStyle={styles.grid}>
            {list.map((app) => (
              <AppTile key={app.id} app={app} onLaunch={launch} />
            ))}
            {!list.length && <Text style={[styles.empty, { color: t.textMute }]}>No apps match “{query}”</Text>}
          </ScrollView>
        </Card>
      </Animated.View>
    </Animated.View>
  );
});

// --------------------------------------------------------- Window switcher

// Anchors (for test drivers) move with the strip: measure again once it rests.
let remeasureTimer = null;
function remeasureSoon() {
  clearTimeout(remeasureTimer);
  remeasureTimer = setTimeout(remeasureAnchors, 150);
}

// The task switcher: every window as a card in a horizontal strip, most recent
// first, flung with the finger and snapping to the card in the middle. The
// current window shrinks into its card when the switcher opens. Tap a card to
// switch, swipe it up to close it, "Split" puts it next to the current window.
const Card_ = memo(function SwitcherCard({ win, card, enter, from, active, title, apps, screen, onPick, onSplit, canSplit }) {
  const t = useTheme();
  const anchor = useAnchor(`switcher.${win.id}`);
  const splitAnchor = useAnchor(`switcher.${win.id}.split`);
  const swipe = useRef(new Animated.Value(0)).current;
  const gone = useRef(false);
  const pan = useMemo(
    () =>
      PanResponder.create({
        onMoveShouldSetPanResponder: (e, g) => g.dy < -8 && Math.abs(g.dy) > Math.abs(g.dx) * 1.2,
        onPanResponderTerminationRequest: () => false,
        onPanResponderMove: (e, g) => swipe.setValue(Math.min(0, g.dy) + Math.max(0, g.dy) * 0.25),
        onPanResponderRelease: (e, g) => {
          if (g.dy < -card.h * 0.3 || g.vy < -0.8) {
            gone.current = true;
            Animated.timing(swipe, { toValue: -screen.height, duration: 220, easing: Easing.out(Easing.quad), useNativeDriver: true }).start();
            wm.close(win.id);
          } else {
            Animated.spring(swipe, { toValue: 0, friction: 7, useNativeDriver: true }).start();
          }
        },
        onPanResponderTerminate: () => Animated.spring(swipe, { toValue: 0, useNativeDriver: true }).start(),
      }),
    [swipe, win.id, card.h, screen.height],
  );
  // Opening: the current window flies from where it was into its card; the
  // others slide in from the right.
  let tx, ty, sc;
  if (from) {
    tx = enter.interpolate({ inputRange: [0, 1], outputRange: [from.cx - card.cx, 0] });
    ty = enter.interpolate({ inputRange: [0, 1], outputRange: [from.cy - card.cy, 0] });
    sc = enter.interpolate({ inputRange: [0, 1], outputRange: [from.s, 1] });
  } else {
    tx = enter.interpolate({ inputRange: [0, 1], outputRange: [screen.width * 0.35, 0] });
    ty = 0;
    sc = enter.interpolate({ inputRange: [0, 1], outputRange: [0.9, 1] });
  }
  const fade = swipe.interpolate({ inputRange: [-screen.height * 0.6, 0], outputRange: [0, 1], extrapolate: 'clamp' });
  const TB = win.decorated ? screen.titleBar * card.s : 0;
  return (
    <Animated.View
      {...pan.panHandlers}
      style={{ width: card.w, marginRight: card.gap, opacity: fade, transform: [{ translateX: tx }, { translateY: Animated.add(ty, swipe) }, { scale: sc }] }}
    >
      <Animated.View style={[styles.cardHead, { opacity: enter }]}>
        <AppIcon win={win} app={apps.find((a) => a.id === win.appId)} size={30} />
        <Text numberOfLines={1} style={styles.cardTitle}>{title}</Text>
      </Animated.View>
      <Pressable ref={anchor.ref} onLayout={anchor.onLayout} onPress={() => onPick(win)}
        style={({ pressed }) => [styles.cardBody, { width: card.w, height: card.h, borderColor: active ? t.accent : 'rgba(255,255,255,0.14)', transform: [{ scale: pressed ? 0.97 : 1 }] }]}>
        {TB > 0 && <View style={{ height: TB, backgroundColor: t.chromeActive }} />}
        <HostTexture name={`win:${win.id}`} style={{ width: card.w, height: card.h - TB }} resizeMode="stretch" />
      </Pressable>
      <Animated.View style={[styles.cardFoot, { opacity: enter }]}>
        {canSplit && (
          <Pressable ref={splitAnchor.ref} onLayout={splitAnchor.onLayout} onPress={() => onSplit(win)}
            style={({ pressed }) => [styles.pill, { marginTop: 0, backgroundColor: pressed ? t.accent : 'rgba(255,255,255,0.14)' }]}>
            <Icon name="snapRight" size={20} color="#fff" />
            <Text style={styles.pillText}>Split</Text>
          </Pressable>
        )}
        <Pressable onPress={() => wm.close(win.id)} hitSlop={6}
          style={({ pressed }) => [styles.pill, { marginTop: 0, marginLeft: 10, width: 44, paddingHorizontal: 0, justifyContent: 'center', backgroundColor: pressed ? t.danger : 'rgba(255,255,255,0.14)' }]}>
          <Icon name="close" size={18} color="#fff" strokeWidth={2.6} />
        </Pressable>
      </Animated.View>
    </Animated.View>
  );
});

export const Overview = memo(function Overview({ screen, windows, active, apps, onClose }) {
  const t = useTheme();
  const enter = useEntrance(true, 300);
  // Most recent first: the stack's top is the window used last.
  const wins = useMemo(
    () => windows.filter((w) => w.type !== 'dock' && w.type !== 'desktop' && w.ready !== false).reverse(),
    [windows],
  );
  const [order] = useState(() => wins.map((w) => w.id)); // keep places while cards close
  const shown = order.map((id) => wins.find((w) => w.id === id)).filter(Boolean)
    .concat(wins.filter((w) => !order.includes(w.id)));
  const areaTop = screen.topBar + 64;
  const areaH = screen.height - areaTop - screen.dock - 20;
  const maxH = Math.round(areaH * 0.74);
  const gap = 44;
  const cards = shown.map((w) => {
    const fh = w.h + (w.decorated ? screen.titleBar : 0);
    let s = maxH / fh;
    if (w.w * s > screen.width * 0.62) s = (screen.width * 0.62) / w.w;
    return { w: Math.round(w.w * s), h: Math.round(fh * s), s, gap };
  });
  const pad = cards.length ? (screen.width - cards[0].w) / 2 : 0;
  const offsets = [];
  let x = pad;
  const cy = areaTop + 40 + maxH / 2;
  cards.forEach((c, i) => {
    c.cx = x + c.w / 2;
    c.cy = cy;
    offsets.push(Math.max(0, c.cx - screen.width / 2));
    x += c.w + gap;
  });
  const pick = (w) => {
    wm.focus(w.id);
    onClose();
  };
  const current = shown.find((w) => w.id === active);
  const split = (w) => {
    if (current && current.id !== w.id) wm.split(current.id, w.id);
    onClose();
  };
  const closeAll = useAnchor('overview.closeAll');
  return (
    <View style={StyleSheet.absoluteFill}>
      <Animated.View style={[StyleSheet.absoluteFill, { opacity: enter }]}>
        <Pressable style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(4,6,16,0.70)' }]} onPress={onClose} />
      </Animated.View>
      <Animated.View pointerEvents="box-none" style={[styles.overviewHead, { top: screen.topBar + 18, opacity: enter }]}>
        <Text style={styles.overviewTitle}>{shown.length ? 'Recent apps' : 'No open apps'}</Text>
        <Text style={styles.overviewHint}>Swipe sideways to browse · tap to switch · swipe a card up to close</Text>
      </Animated.View>
      {shown.length > 0 && (
        <ScrollView
          horizontal
          showsHorizontalScrollIndicator={false}
          snapToOffsets={offsets}
          decelerationRate="fast"
          onScroll={remeasureSoon}
          scrollEventThrottle={50}
          style={{ position: 'absolute', left: 0, right: 0, top: areaTop, height: maxH + 140 }}
          contentContainerStyle={{ paddingLeft: pad, paddingRight: pad, paddingTop: 0, alignItems: 'flex-start' }}
        >
          {shown.map((w, i) => {
            const c = cards[i];
            let from = null;
            if (i === 0) {
              // Where the window was on the desktop, relative to its card.
              const fh = w.h + (w.decorated ? screen.titleBar : 0);
              from = { cx: w.x + w.w / 2, cy: w.y - (w.decorated ? screen.titleBar : 0) + fh / 2, s: w.w / c.w };
            }
            const card = { ...c, cy: areaTop + 46 + c.h / 2 };
            return (
              <Card_ key={w.id} win={w} card={card} enter={enter} from={from} active={w.id === active} screen={screen}
                title={w.title || appTitle(w, apps)} apps={apps} onPick={pick} onSplit={split}
                canSplit={!!current && current.id !== w.id && w.type === 'normal' && current.type === 'normal'} />
            );
          })}
        </ScrollView>
      )}
      {shown.length > 1 && (
        <Animated.View pointerEvents="box-none" style={[styles.overviewFoot, { bottom: screen.dock + 8, opacity: enter }]}>
          <Pressable ref={closeAll.ref} onLayout={closeAll.onLayout} onPress={() => wm.closeAll()}
            style={({ pressed }) => [styles.pill, { backgroundColor: pressed ? t.danger : 'rgba(255,255,255,0.14)' }]}>
            <Icon name="close" size={18} color="#fff" />
            <Text style={styles.pillText}>Close all</Text>
          </Pressable>
        </Animated.View>
      )}
    </View>
  );
});

// --------------------------------------------------------- Quick settings

export const QuickSettings = memo(function QuickSettings({ screen, settings, system, windows, onClose, onLock, onReload }) {
  const t = useTheme();
  const v = useEntrance(true, 200);
  const ty = v.interpolate({ inputRange: [0, 1], outputRange: [-24, 0] });
  const set = (k) => () => setSetting(k, !settings[k]);
  const a = {
    wifi: useAnchor('qs.wifi'),
    dark: useAnchor('qs.dark'),
    tablet: useAnchor('qs.tablet'),
    night: useAnchor('qs.night'),
    brightness: useAnchor('qs.brightness'),
  };
  return (
    <View style={StyleSheet.absoluteFill}>
      <Pressable style={StyleSheet.absoluteFill} onPress={onClose} />
      <Animated.View style={[styles.dropRight, { top: screen.topBar + 10, opacity: v, transform: [{ translateY: ty }] }]}>
        <Card style={{ width: 420 }}>
          <View style={styles.qsHead}>
            <View style={[styles.avatar, { backgroundColor: t.accent }]}><Icon name="user" size={24} color="#fff" /></View>
            <View style={{ flex: 1, marginLeft: 12 }}>
              <Text style={[styles.qsUser, { color: t.text }]}>{system.user}</Text>
              <Text style={[styles.qsHost, { color: t.textMute }]}>{system.hostname}{system.battery ? ` · ${system.battery.percent}%${system.battery.charging ? ' charging' : ''}` : ' · on AC power'}</Text>
            </View>
            <IconButton icon="lock" size={48} iconSize={22} onPress={onLock} label="Lock" />
            <IconButton icon="refresh" size={48} iconSize={22} onPress={onReload} label="Restart shell" />
            <IconButton icon="logout" size={48} iconSize={22} onPress={() => wm.quit()} label="Log out" color={t.danger} />
          </View>
          <View style={styles.tiles}>
            <ToggleTile anchor={a.wifi} icon="wifi" label="Wi-Fi" sub={settings.wifi ? 'Studio 5G' : 'Off'} on={settings.wifi} onPress={set('wifi')} />
            <ToggleTile icon="bluetooth" label="Bluetooth" on={settings.bluetooth} onPress={set('bluetooth')} />
            <ToggleTile anchor={a.dark} icon="moon" label="Dark style" on={settings.darkMode} onPress={set('darkMode')} />
            <ToggleTile anchor={a.tablet} icon="tablet" label="Tablet mode" sub={settings.tabletMode ? 'Apps open full' : 'Floating windows'} on={settings.tabletMode} onPress={() => {
              const on = !settings.tabletMode;
              setSetting('tabletMode', on);
              if (on) for (const w of windows) if (w.type === 'normal' && !w.transientFor && !w.minimized && !w.snap) wm.maximize(w.id, true);
            }} />
            <ToggleTile anchor={a.night} icon="sun" label="Night light" on={settings.nightLight} onPress={set('nightLight')} />
            <ToggleTile icon="bell" label="Do not disturb" on={settings.doNotDisturb} onPress={set('doNotDisturb')} />
          </View>
          <Slider anchor={a.brightness} icon="sun" value={settings.brightness} onChange={(x) => setSetting('brightness', Math.max(0.2, x))} />
          <Slider icon={settings.muted ? 'mute' : 'volume'} value={settings.volume} onChange={(x) => setSetting('volume', x)} />
        </Card>
      </Animated.View>
    </View>
  );
});

// ------------------------------------------------- Calendar + notifications

function monthGrid(d) {
  const first = new Date(d.getFullYear(), d.getMonth(), 1);
  const start = (first.getDay() + 6) % 7; // Monday first
  const days = new Date(d.getFullYear(), d.getMonth() + 1, 0).getDate();
  const cells = [];
  for (let i = 0; i < start; i++) cells.push(null);
  for (let i = 1; i <= days; i++) cells.push(i);
  while (cells.length % 7) cells.push(null);
  return cells;
}

export const CalendarPanel = memo(function CalendarPanel({ screen, notifications, onClear, onClose }) {
  const t = useTheme();
  const now = useClock();
  const v = useEntrance(true, 200);
  const ty = v.interpolate({ inputRange: [0, 1], outputRange: [-24, 0] });
  const cells = monthGrid(now);
  return (
    <View style={StyleSheet.absoluteFill}>
      <Pressable style={StyleSheet.absoluteFill} onPress={onClose} />
      <Animated.View style={[styles.dropCenter, { top: screen.topBar + 10, opacity: v, transform: [{ translateY: ty }] }]}>
        <Card style={{ width: 720, flexDirection: 'row' }}>
          <View style={{ flex: 1, paddingRight: 18 }}>
            <Label>NOTIFICATIONS</Label>
            {notifications.length === 0 && <Text style={[styles.empty, { color: t.textMute, marginTop: 30 }]}>You're all caught up</Text>}
            <ScrollView style={{ maxHeight: 330, marginTop: 8 }}>
              {notifications.map((n) => (
                <View key={n.id} style={[styles.note, { backgroundColor: t.panelRaised }]}>
                  <Icon name={n.icon || 'bell'} size={22} color={t.accent} />
                  <View style={{ flex: 1, marginLeft: 12 }}>
                    <Text numberOfLines={1} style={[styles.noteTitle, { color: t.text }]}>{n.title}</Text>
                    {n.body ? <Text numberOfLines={2} style={[styles.noteBody, { color: t.textDim }]}>{n.body}</Text> : null}
                  </View>
                  <Text style={[styles.noteTime, { color: t.textMute }]}>{n.time}</Text>
                </View>
              ))}
            </ScrollView>
            {notifications.length > 0 && (
              <Pressable onPress={onClear} style={({ pressed }) => [styles.pill, { alignSelf: 'flex-start', marginTop: 10, backgroundColor: pressed ? t.panelLine : t.panelRaised }]}>
                <Text style={[styles.pillText, { color: t.text, marginLeft: 0 }]}>Clear all</Text>
              </Pressable>
            )}
          </View>
          <View style={{ width: 1, backgroundColor: t.panelLine }} />
          <View style={{ width: 320, paddingLeft: 18 }}>
            <Text style={[styles.bigTime, { color: t.text }]}>{timeText(now)}</Text>
            <Text style={[styles.bigDate, { color: t.textDim }]}>{dateText(now)} {now.getFullYear()}</Text>
            <Label style={{ marginTop: 16, marginBottom: 6 }}>{longMonth(now).toUpperCase()}</Label>
            <View style={styles.calRow}>
              {['M', 'T', 'W', 'T', 'F', 'S', 'S'].map((d, i) => (
                <Text key={i} style={[styles.calHead, { color: t.textMute }]}>{d}</Text>
              ))}
            </View>
            <View style={styles.calGrid}>
              {cells.map((c, i) => (
                <View key={i} style={[styles.calCell, c === now.getDate() && { backgroundColor: t.accent }]}>
                  <Text style={[styles.calText, { color: c === now.getDate() ? t.accentText : t.text }]}>{c || ''}</Text>
                </View>
              ))}
            </View>
          </View>
        </Card>
      </Animated.View>
    </View>
  );
});

// --------------------------------------------------------------- Lock screen

export const LockScreen = memo(function LockScreen({ screen, onUnlock, notifications }) {
  const now = useClock();
  const drag = useRef(new Animated.Value(0)).current;
  const v = useEntrance(true, 260);
  const anchor = useAnchor('lock.handle');
  const pan = useMemo(
    () =>
      PanResponder.create({
        onStartShouldSetPanResponder: () => true,
        onMoveShouldSetPanResponder: () => true,
        onPanResponderMove: (e, g) => drag.setValue(Math.min(0, g.dy)),
        onPanResponderRelease: (e, g) => {
          if (g.dy < -140 || g.vy < -0.9) {
            Animated.timing(drag, { toValue: -screen.height, duration: 260, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start(onUnlock);
          } else {
            Animated.spring(drag, { toValue: 0, useNativeDriver: true }).start();
          }
        },
      }),
    [drag, onUnlock, screen.height],
  );
  const fade = drag.interpolate({ inputRange: [-400, 0], outputRange: [0, 1], extrapolate: 'clamp' });
  return (
    <Animated.View {...pan.panHandlers} style={[StyleSheet.absoluteFill, { opacity: Animated.multiply(v, fade), transform: [{ translateY: drag }] }]}>
      <View style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(6,8,22,0.55)' }]} />
      <View style={styles.lockBody}>
        <Text style={styles.lockTime}>{pad2(now.getHours())}:{pad2(now.getMinutes())}</Text>
        <Text style={styles.lockDate}>{dateText(now)}</Text>
        {notifications.length > 0 && (
          <View style={styles.lockNote}>
            <Icon name="bell" size={18} color="#fff" />
            <Text style={styles.lockNoteText}>{notifications.length} notification{notifications.length === 1 ? '' : 's'}</Text>
          </View>
        )}
      </View>
      <View ref={anchor.ref} onLayout={anchor.onLayout} style={styles.lockHandle}>
        <Icon name="chevronUp" size={30} color="#fff" />
        <Text style={styles.lockHint}>Swipe up to unlock</Text>
      </View>
    </Animated.View>
  );
});

// ------------------------------------------------------------------ Toasts

export const Toasts = memo(function Toasts({ screen, toasts }) {
  const t = useTheme();
  return (
    <View pointerEvents="none" style={[styles.toasts, { top: screen.topBar + 12 }]}>
      {toasts.map((n) => (
        <Toast key={n.id} n={n} t={t} />
      ))}
    </View>
  );
});

function Toast({ n, t }) {
  const v = useEntrance(true, 240);
  const tx = v.interpolate({ inputRange: [0, 1], outputRange: [60, 0] });
  return (
    <Animated.View style={[styles.toast, { backgroundColor: t.panel, borderColor: t.panelLine, boxShadow: `0px 12px 32px ${t.shadow}`, opacity: v, transform: [{ translateX: tx }] }]}>
      <Icon name={n.icon || 'bell'} size={22} color={t.accent} />
      <View style={{ flex: 1, marginLeft: 12 }}>
        <Text numberOfLines={1} style={[styles.noteTitle, { color: t.text }]}>{n.title}</Text>
        {n.body ? <Text numberOfLines={2} style={[styles.noteBody, { color: t.textDim }]}>{n.body}</Text> : null}
      </View>
    </Animated.View>
  );
}

const styles = StyleSheet.create({
  launcher: { position: 'absolute', left: 0, right: 0, alignItems: 'center' },
  search: { width: 620, height: 64, borderRadius: 32, borderWidth: 1, flexDirection: 'row', alignItems: 'center', paddingLeft: 22, paddingRight: 8 },
  searchInput: { flex: 1, height: 56, fontFamily: font, fontSize: 20, marginLeft: 12 },
  grid: { flexDirection: 'row', flexWrap: 'wrap', paddingBottom: 12 },
  appTile: { width: 150, height: 150, borderRadius: 24, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 8 },
  appName: { fontFamily: font, fontSize: 15, fontWeight: '500', marginTop: 10, textAlign: 'center' },
  empty: { fontFamily: font, fontSize: 16, textAlign: 'center', width: '100%', marginTop: 40 },
  overviewHead: { position: 'absolute', left: 0, right: 0, alignItems: 'center' },
  overviewTitle: { fontFamily: font, color: '#fff', fontSize: 26, fontWeight: '700' },
  overviewHint: { fontFamily: font, color: 'rgba(255,255,255,0.7)', fontSize: 15, marginTop: 4 },
  pill: { flexDirection: 'row', alignItems: 'center', height: 44, borderRadius: 22, paddingHorizontal: 18, marginTop: 12 },
  pillText: { fontFamily: font, color: '#fff', fontSize: 15, fontWeight: '600', marginLeft: 8 },
  cardHead: { height: 46, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 4 },
  cardTitle: { flex: 1, fontFamily: font, color: '#fff', fontSize: 17, fontWeight: '600', marginLeft: 10 },
  cardBody: { borderRadius: 22, overflow: 'hidden', borderWidth: 3, boxShadow: '0px 24px 60px rgba(0,0,0,0.55)' },
  cardFoot: { height: 64, flexDirection: 'row', alignItems: 'center', justifyContent: 'center' },
  overviewFoot: { position: 'absolute', left: 0, right: 0, alignItems: 'center' },
  thumbTitle: { height: 40, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 2 },
  thumbText: { flex: 1, fontFamily: font, fontSize: 15, fontWeight: '600', marginLeft: 10 },
  thumbClose: { width: 32, height: 32, borderRadius: 16, backgroundColor: 'rgba(255,255,255,0.18)', alignItems: 'center', justifyContent: 'center' },
  thumb: { borderRadius: 16, overflow: 'hidden', borderWidth: 3 },
  dropRight: { position: 'absolute', right: 12 },
  dropCenter: { position: 'absolute', left: 0, right: 0, alignItems: 'center' },
  qsHead: { flexDirection: 'row', alignItems: 'center', marginBottom: 14 },
  avatar: { width: 48, height: 48, borderRadius: 24, alignItems: 'center', justifyContent: 'center' },
  qsUser: { fontFamily: font, fontSize: 17, fontWeight: '700' },
  qsHost: { fontFamily: font, fontSize: 13, marginTop: 2 },
  tiles: { flexDirection: 'row', flexWrap: 'wrap', gap: 10, marginBottom: 10 },
  note: { flexDirection: 'row', alignItems: 'center', borderRadius: 18, padding: 14, marginBottom: 8 },
  noteTitle: { fontFamily: font, fontSize: 15, fontWeight: '600' },
  noteBody: { fontFamily: font, fontSize: 13, marginTop: 2 },
  noteTime: { fontFamily: font, fontSize: 12, marginLeft: 8 },
  bigTime: { fontFamily: font, fontSize: 52, fontWeight: '700', letterSpacing: -1 },
  bigDate: { fontFamily: font, fontSize: 16, fontWeight: '500' },
  calRow: { flexDirection: 'row' },
  calHead: { width: 44, textAlign: 'center', fontFamily: font, fontSize: 12, fontWeight: '700' },
  calGrid: { flexDirection: 'row', flexWrap: 'wrap', width: 308 },
  calCell: { width: 44, height: 38, borderRadius: 19, alignItems: 'center', justifyContent: 'center' },
  calText: { fontFamily: font, fontSize: 14, fontWeight: '500' },
  lockBody: { position: 'absolute', left: 0, right: 0, top: '18%', alignItems: 'center' },
  lockTime: { fontFamily: font, color: '#fff', fontSize: 132, fontWeight: '700', letterSpacing: -4 },
  lockDate: { fontFamily: font, color: 'rgba(255,255,255,0.85)', fontSize: 26, fontWeight: '500' },
  lockNote: { flexDirection: 'row', alignItems: 'center', marginTop: 28, backgroundColor: 'rgba(255,255,255,0.14)', borderRadius: 22, height: 44, paddingHorizontal: 18 },
  lockNoteText: { fontFamily: font, color: '#fff', fontSize: 15, marginLeft: 10 },
  lockHandle: { position: 'absolute', left: 0, right: 0, bottom: 40, alignItems: 'center' },
  lockHint: { fontFamily: font, color: 'rgba(255,255,255,0.85)', fontSize: 17, marginTop: 4, fontWeight: '500' },
  toasts: { position: 'absolute', right: 16, width: 380 },
  toast: { flexDirection: 'row', alignItems: 'center', borderRadius: 20, borderWidth: 1, padding: 16, marginBottom: 10 },
});
