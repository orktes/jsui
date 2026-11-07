// Full-screen and drop-down panels: the app launcher, the window overview,
// quick settings, the calendar / notifications panel and the lock screen.
import React, { memo, useEffect, useMemo, useRef, useState } from 'react';
import {
  Animated, Easing, HostTexture, PanResponder, Pressable, ScrollView, StyleSheet, Text, TextInput, View,
} from 'react-native';
import { remeasureAnchors, useAnchor } from './anchors.js';
import { useNativeDragY } from './drag.js';
import { AppIcon } from './AppIcon.jsx';
import { dateText, longMonth, pad2, timeText, useClock } from './bars.jsx';
import { Card, IconButton, Label, Slider, ToggleTile } from './controls.jsx';
import { Icon } from './icons.jsx';
import { font, useTheme } from './theme.js';
import { appName, cleanName, setSetting, wm } from './wm.js';

// Animates 0 -> 1 on mount, and back to 0 (faster, easing in) when `open`
// goes false, then calls onClosed so the shell can unmount the panel.
function useEntrance(open = true, duration = 220, onClosed) {
  const v = useRef(new Animated.Value(0)).current;
  const done = useRef(onClosed);
  done.current = onClosed;
  useEffect(() => {
    if (open) {
      Animated.timing(v, { toValue: 1, duration, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
    } else {
      Animated.timing(v, { toValue: 0, duration: 170, easing: Easing.in(Easing.cubic), useNativeDriver: true }).start(() => done.current && done.current());
    }
  }, [open, v, duration]);
  return v;
}

// A spring 0 -> 1 for things that should land with a little life.
function useSpringIn(open = true) {
  const v = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    if (open) Animated.spring(v, { toValue: 1, tension: 180, friction: 22, useNativeDriver: true }).start();
    else Animated.timing(v, { toValue: 0, duration: 170, easing: Easing.in(Easing.cubic), useNativeDriver: true }).start();
  }, [open, v]);
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
      style={styles.appTile} pressedStyle={{ backgroundColor: 'rgba(255,255,255,0.12)', transform: [{ scale: 0.95 }] }}
    >
      <AppIcon app={app} size={72} />
      <Text numberOfLines={2} style={[styles.appName, { color: '#fff' }]}>{cleanName(app.name)}</Text>
    </Pressable>
  );
});

// A full-screen panel's dimmed background. With the host's blurred snapshot of
// the screen ("wm:backdrop", opaque; jsui-wl, unless JSUI_WL_SNAPSHOT_BG=0) it is
// drawn over that instead of over the live desktop, so nothing below it is
// painted once the panel is fully shown.
// The screen as it was when the panel opened ("wm:under", opaque), drawn under
// the panel's fade: while the panel is up nothing below it is painted (the
// live desktop would be the same pixels).
function Under({ screen }) {
  return (
    <HostTexture name="wm:under" resizeMode="stretch" pointerEvents="none"
      style={{ position: 'absolute', left: 0, top: 0, width: screen.width, height: screen.height }} />
  );
}

function Scrim({ color, onPress, screen }) {
  // Without the texture (not set yet, or turned off) the view draws nothing.
  // Sized explicitly: a HostTexture otherwise takes the texture's size.
  return (
    <Pressable style={StyleSheet.absoluteFill} onPress={onPress}>
      <HostTexture name="wm:backdrop" resizeMode="stretch" pointerEvents="none"
        style={{ position: 'absolute', left: 0, top: 0, width: screen.width, height: screen.height }} />
      <View pointerEvents="none" style={[StyleSheet.absoluteFill, { backgroundColor: color }]} />
    </Pressable>
  );
}

export const Launcher = memo(function Launcher({ screen, apps, onClose, onClosed, onLaunch, settings, open = true }) {
  const t = useTheme();
  const [query, setQuery] = useState('');
  const v = useEntrance(open, 200, onClosed);
  const pop = useSpringIn(open);
  const input = useRef(null);
  useEffect(() => {
    if (!open) {
      setQuery(''); // kept mounted between opens (shell.jsx): start afresh
      return undefined;
    }
    wm.keyboard(true); // the search field gets the physical keyboard
    const id = setTimeout(() => input.current && input.current.focus && input.current.focus(), 50);
    return () => {
      clearTimeout(id);
      wm.keyboard(false);
    };
  }, [open]);
  const list = useMemo(() => {
    const q = query.trim().toLowerCase();
    if (!q) return apps;
    return apps.filter((a) => a.name.toLowerCase().includes(q) || (a.comment || '').toLowerCase().includes(q) || a.exec.toLowerCase().includes(q));
  }, [apps, query]);
  const launch = (app) => {
    onLaunch(app);
    onClose();
  };
  const sc = pop.interpolate({ inputRange: [0, 1], outputRange: [0.96, 1] });
  const cols = Math.max(3, Math.min(8, Math.floor((screen.width - 160) / 150)));
  return (
    <View pointerEvents="box-none" style={StyleSheet.absoluteFill}>
      <Under screen={screen} />
      <Animated.View pointerEvents={open ? 'auto' : 'none'} style={[StyleSheet.absoluteFill, { opacity: v }]}>
        <Scrim color="rgba(6,8,20,0.82)" onPress={onClose} screen={screen} />
        <Animated.View pointerEvents="box-none" style={[styles.launcher, { top: screen.topBar + 28, bottom: settings.osk ? 330 : screen.dock + 12, transform: [{ scale: sc }] }]}>
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
          <View style={{ flex: 1, marginTop: 28, width: cols * 150 }}>
            {query ? <Label style={{ marginBottom: 8, marginLeft: 10, color: 'rgba(255,255,255,0.7)' }}>{`${list.length} RESULT${list.length === 1 ? '' : 'S'}`}</Label> : null}
            <ScrollView contentContainerStyle={styles.grid} showsVerticalScrollIndicator={false}>
              {list.map((app) => (
                <AppTile key={app.id} app={app} onLaunch={launch} />
              ))}
              {!list.length && <Text style={[styles.empty, { color: 'rgba(255,255,255,0.6)' }]}>No apps match “{query}”</Text>}
            </ScrollView>
          </View>
        </Animated.View>
      </Animated.View>
    </View>
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
// current window shrinks into its card when the switcher opens, and the card
// you pick grows back into the full window when it closes. Tap a card to
// switch, swipe it up to close it, "Split" puts it next to the current window.
const SwitcherCard = memo(function SwitcherCard({ win, card, enter, from, title, apps, screen, onPick, onSplit, canSplit }) {
  const t = useTheme();
  const anchor = useAnchor(`switcher.${win.id}`);
  const splitAnchor = useAnchor(`switcher.${win.id}.split`);
  // The swipe: the finger natively (up 1:1, down damped to a quarter) while it
  // is down, then `swipe` (an animation from where it let go).
  const swipe = useRef(new Animated.Value(0)).current;
  const drag = useNativeDragY();
  const shape = (dy) => Math.min(0, dy) + Math.max(0, dy) * 0.25;
  const follow = useMemo(
    () => drag.dy.interpolate({ inputRange: [-1, 0, 1], outputRange: [-1, 0, 0.25], extrapolate: 'extend' }),
    [drag],
  );
  const lift = useMemo(() => Animated.add(follow, swipe), [follow, swipe]);
  const handOver = (dy) => {
    swipe.setValue(shape(dy));
    drag.on.setValue(0);
  };
  const pan = useMemo(
    () =>
      PanResponder.create({
        onMoveShouldSetPanResponder: (e, g) => g.dy < -8 && Math.abs(g.dy) > Math.abs(g.dx) * 1.2,
        onPanResponderTerminationRequest: () => false,
        onPanResponderGrant: () => drag.on.setValue(1),
        onPanResponderRelease: (e, g) => {
          handOver(g.dy);
          if (g.dy < -card.h * 0.3 || g.vy < -0.8) {
            Animated.timing(swipe, { toValue: -screen.height, duration: 220, easing: Easing.out(Easing.quad), useNativeDriver: true }).start();
            wm.close(win.id);
          } else {
            Animated.spring(swipe, { toValue: 0, tension: 160, friction: 16, useNativeDriver: true }).start();
          }
        },
        onPanResponderTerminate: (e, g) => {
          handOver(g.dy);
          Animated.spring(swipe, { toValue: 0, useNativeDriver: true }).start();
        },
      }),
    [swipe, drag, win.id, card.h, screen.height],
  );
  // The card that maps to the desktop flies between its window's place and the
  // strip; the others slide in from the right.
  let tx, ty, sc;
  if (from) {
    tx = enter.interpolate({ inputRange: [0, 1], outputRange: [from.cx - card.screenCx, 0] });
    ty = enter.interpolate({ inputRange: [0, 1], outputRange: [from.cy - card.cy, 0] });
    sc = enter.interpolate({ inputRange: [0, 1], outputRange: [from.s, 1] });
  } else {
    tx = enter.interpolate({ inputRange: [0, 1], outputRange: [screen.width * 0.35, 0] });
    ty = 0;
    sc = enter.interpolate({ inputRange: [0, 1], outputRange: [0.9, 1] });
  }
  const fade = lift.interpolate({ inputRange: [-screen.height * 0.6, 0], outputRange: [0, 1], extrapolate: 'clamp' });
  const radius = enter.interpolate({ inputRange: [0, 1], outputRange: [from ? 0 : 22, 22] });
  return (
    <Animated.View
      {...pan.panHandlers}
      {...drag.handlers}
      style={{ width: card.w, marginRight: card.gap, opacity: fade, transform: [{ translateX: tx }, { translateY: Animated.add(ty, lift) }] }}
    >
      <Animated.View style={[styles.cardHead, { opacity: enter }]}>
        <AppIcon win={win} app={apps.find((a) => a.id === win.appId)} size={32} />
        <Text numberOfLines={1} style={styles.cardTitle}>{title}</Text>
        {canSplit && (
          <Pressable ref={splitAnchor.ref} onLayout={splitAnchor.onLayout} onPress={() => onSplit(win)}
            style={[styles.headPill, { backgroundColor: 'rgba(255,255,255,0.14)' }]} pressedStyle={{ backgroundColor: t.accent }}>
            <Icon name="snapRight" size={18} color="#fff" />
            <Text style={styles.pillText}>Split</Text>
          </Pressable>
        )}
        <Pressable onPress={() => wm.close(win.id)} hitSlop={8}
          style={[styles.headClose, { backgroundColor: 'rgba(255,255,255,0.14)' }]} pressedStyle={{ backgroundColor: t.danger }}>
          <Icon name="close" size={16} color="#fff" strokeWidth={2.6} />
        </Pressable>
      </Animated.View>
      <Animated.View style={{ transform: [{ scale: sc }] }}>
        <Pressable ref={anchor.ref} onLayout={anchor.onLayout} onPress={() => onPick(win)}
          pressedStyle={{ transform: [{ scale: 0.97 }] }}>
          <Animated.View style={[styles.cardBody, { width: card.w, height: card.h, borderRadius: radius }]}>
            <HostTexture name={`win:${win.id}`} style={{ width: card.w, height: card.h }} resizeMode="stretch" />
          </Animated.View>
        </Pressable>
      </Animated.View>
    </Animated.View>
  );
});

export const Overview = memo(function Overview({ screen, windows, active, apps, onClose, onClosed, open = true }) {
  const t = useTheme();
  // Which window the switcher grows back into when it closes: the current
  // one, or the card that was tapped.
  const [target, setTarget] = useState(active);
  const enter = useEntrance(open, 300, onClosed);
  const scrollX = useRef(0);
  // Most recent first: the stack's top is the window used last.
  const wins = useMemo(
    () => windows.filter((w) => w.type !== 'dock' && w.type !== 'desktop' && w.ready !== false).reverse(),
    [windows],
  );
  const [order] = useState(() => wins.map((w) => w.id)); // keep places while cards close
  const shown = order.map((id) => wins.find((w) => w.id === id)).filter(Boolean)
    .concat(wins.filter((w) => !order.includes(w.id)));
  const headH = 60;
  const areaTop = screen.topBar + 84;
  const areaH = screen.height - areaTop - screen.dock - 24;
  const maxH = Math.round(Math.min(areaH - headH, areaH * 0.8));
  const gap = 48;
  const cards = shown.map((w) => {
    let s = maxH / w.h;
    if (w.w * s > screen.width * 0.62) s = (screen.width * 0.62) / w.w;
    return { w: Math.round(w.w * s), h: Math.round(w.h * s), s, gap };
  });
  const pad = cards.length ? (screen.width - cards[0].w) / 2 : 0;
  const offsets = [];
  let x = pad;
  cards.forEach((c) => {
    c.cx = x + c.w / 2;
    c.screenCx = c.cx - scrollX.current;
    c.cy = areaTop + headH + c.h / 2;
    offsets.push(Math.max(0, c.cx - screen.width / 2));
    x += c.w + gap;
  });
  const pick = (w) => {
    setTarget(w.id);
    wm.focus(w.id);
    onClose();
  };
  const current = shown.find((w) => w.id === active);
  const split = (w) => {
    if (current && current.id !== w.id) wm.split(current.id, w.id);
    setTarget(null);
    onClose();
  };
  const closeAll = useAnchor('overview.closeAll');
  return (
    <View pointerEvents={open ? 'auto' : 'none'} style={StyleSheet.absoluteFill}>
      <Under screen={screen} />
      <Animated.View style={[StyleSheet.absoluteFill, { opacity: enter }]}>
        <Scrim color="rgba(4,6,16,0.72)" onPress={onClose} screen={screen} />
      </Animated.View>
      <Animated.View pointerEvents="box-none" style={[styles.overviewHead, { top: screen.topBar + 22, opacity: enter }]}>
        <Text style={styles.overviewTitle}>{shown.length ? 'Recent apps' : 'No open apps'}</Text>
        {shown.length > 1 && (
          <Pressable ref={closeAll.ref} onLayout={closeAll.onLayout} onPress={() => wm.closeAll()} hitSlop={8}
            style={styles.textButton} pressedStyle={{ backgroundColor: 'rgba(255,255,255,0.14)' }}>
            <Text style={styles.textButtonText}>Close all</Text>
          </Pressable>
        )}
      </Animated.View>
      {shown.length > 0 && (
        <ScrollView
          horizontal
          showsHorizontalScrollIndicator={false}
          snapToOffsets={offsets}
          decelerationRate="fast"
          onScroll={(e) => {
            scrollX.current = e.nativeEvent.contentOffset.x;
            remeasureSoon();
          }}
          scrollEventThrottle={16}
          style={{ position: 'absolute', left: 0, right: 0, top: areaTop, height: maxH + headH + 40 }}
          contentContainerStyle={{ paddingLeft: pad, paddingRight: pad, alignItems: 'flex-start' }}
        >
          {shown.map((w, i) => {
            const c = cards[i];
            const from = w.id === target && !w.minimized ? { cx: w.x + w.w / 2, cy: w.y + w.h / 2, s: w.w / c.w } : null;
            return (
              <SwitcherCard key={w.id} win={w} card={c} enter={enter} from={from} screen={screen}
                title={appName(w, apps)} apps={apps} onPick={pick} onSplit={split}
                canSplit={!!current && current.id !== w.id && w.type === 'normal' && current.type === 'normal'} />
            );
          })}
        </ScrollView>
      )}
    </View>
  );
});

// --------------------------------------------------------- Quick settings

export const QuickSettings = memo(function QuickSettings({ screen, settings, system, windows, onClose, onClosed, onLock, onReload, open = true }) {
  const t = useTheme();
  const v = useEntrance(open, 200, onClosed);
  const [power, setPower] = useState(false);
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
    <View pointerEvents={open ? 'auto' : 'none'} style={StyleSheet.absoluteFill}>
      <Animated.View style={[StyleSheet.absoluteFill, { opacity: v }]}>
        <Pressable style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(0,0,0,0.25)' }]} onPress={onClose} />
      </Animated.View>
      <Animated.View style={[styles.dropRight, { top: screen.topBar + 10, opacity: v, transform: [{ translateY: ty }] }]}>
        <Card style={{ width: 420 }}>
          <View style={styles.qsHead}>
            <View style={[styles.avatar, { backgroundColor: t.accent }]}><Icon name="user" size={24} color="#fff" /></View>
            <View style={{ flex: 1, marginLeft: 12 }}>
              <Text style={[styles.qsUser, { color: t.text }]}>{system.user}</Text>
              <Text style={[styles.qsHost, { color: t.textMute }]}>{system.hostname}{system.battery ? ` · ${system.battery.percent}%${system.battery.charging ? ' charging' : ''}` : ' · on AC power'}</Text>
            </View>
            <IconButton icon="lock" size={48} iconSize={22} onPress={onLock} label="Lock" />
            <IconButton icon="power" size={48} iconSize={22} active={power} onPress={() => setPower(!power)} label="Power" />
          </View>
          {power ? (
            <View style={{ marginBottom: 6 }}>
              {[
                { icon: 'lock', label: 'Lock', fn: onLock },
                { icon: 'refresh', label: 'Restart shell', fn: onReload },
                { icon: 'logout', label: 'Log out', fn: () => wm.quit() },
              ].map((r) => (
                <Pressable key={r.label} onPress={r.fn}
                  style={[styles.powerRow, { backgroundColor: t.panelRaised }]} pressedStyle={{ backgroundColor: t.panelLine }}>
                  <Icon name={r.icon} size={22} color={t.text} />
                  <Text style={[styles.powerText, { color: t.text }]}>{r.label}</Text>
                </Pressable>
              ))}
            </View>
          ) : null}
          {!power && <View style={styles.tiles}>
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
          </View>}
          {!power && <Slider anchor={a.brightness} icon="sun" value={settings.brightness} onChange={(x) => setSetting('brightness', Math.max(0.2, x))} />}
          {!power && <Slider icon={settings.muted ? 'mute' : 'volume'} value={settings.volume} onChange={(x) => setSetting('volume', x)} />}
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

export const CalendarPanel = memo(function CalendarPanel({ screen, notifications, onClear, onClose, onClosed, open = true }) {
  const t = useTheme();
  const now = useClock();
  const v = useEntrance(open, 200, onClosed);
  const ty = v.interpolate({ inputRange: [0, 1], outputRange: [-24, 0] });
  const cells = monthGrid(now);
  return (
    <View pointerEvents={open ? 'auto' : 'none'} style={StyleSheet.absoluteFill}>
      <Animated.View style={[StyleSheet.absoluteFill, { opacity: v }]}>
        <Pressable style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(0,0,0,0.25)' }]} onPress={onClose} />
      </Animated.View>
      <Animated.View style={[styles.dropCenter, { top: screen.topBar + 10, opacity: v, transform: [{ translateY: ty }] }]}>
        <Card style={{ width: 720, flexDirection: 'row' }}>
          <View style={{ flex: 1, paddingRight: 18 }}>
            <Label>NOTIFICATIONS</Label>
            {notifications.length === 0 && <Text style={[styles.empty, { color: t.textMute, marginTop: 30 }]}>You're all caught up</Text>}
            <View style={{ maxHeight: 340, marginTop: 8 }}>
            <ScrollView style={{ maxHeight: 340 }} contentContainerStyle={{ paddingBottom: 24 }}>
              {notifications.map((n) => (
                <View key={n.id} style={[styles.note, { backgroundColor: t.panelRaised }]}>
                  {n.app ? <AppIcon app={n.app} size={32} /> : <Icon name={n.icon || 'bell'} size={22} color={t.accent} />}
                  <View style={{ flex: 1, marginLeft: 12 }}>
                    <Text numberOfLines={1} style={[styles.noteTitle, { color: t.text }]}>{n.title}</Text>
                    {n.body ? <Text numberOfLines={2} style={[styles.noteBody, { color: t.textDim }]}>{n.body}</Text> : null}
                  </View>
                  <Text style={[styles.noteTime, { color: t.textMute }]}>{n.time}</Text>
                </View>
              ))}
            </ScrollView>
            <View pointerEvents="none" style={[styles.fadeBottom, { experimental_backgroundImage: `linear-gradient(180deg, rgba(0,0,0,0) 0%, ${t.panel} 100%)` }]} />
            </View>
            {notifications.length > 0 && (
              <Pressable onPress={onClear} style={[styles.pill, { alignSelf: 'flex-start', marginTop: 10, backgroundColor: t.panelRaised }]} pressedStyle={{ backgroundColor: t.panelLine }}>
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
                <Text key={i} style={[styles.calHead, { color: i >= 5 ? t.textMute : t.textDim }]}>{d}</Text>
              ))}
            </View>
            <View style={styles.calGrid}>
              {cells.map((c, i) => (
                <View key={i} style={[styles.calCell, c === now.getDate() && { backgroundColor: t.accent }]}>
                  <Text style={[styles.calText, { color: c === now.getDate() ? t.accentText : i % 7 >= 5 ? t.textMute : t.text }]}>{c || ''}</Text>
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
  // Up with the finger natively while it is down, then `drag` (an animation
  // from where it let go).
  const drag = useRef(new Animated.Value(0)).current;
  const finger = useNativeDragY();
  const up = useMemo(
    () => Animated.add(finger.dy.interpolate({ inputRange: [-1, 0], outputRange: [-1, 0], extrapolateLeft: 'extend', extrapolateRight: 'clamp' }), drag),
    [finger, drag],
  );
  const v = useEntrance(true, 260);
  const anchor = useAnchor('lock.handle');
  const pan = useMemo(
    () =>
      PanResponder.create({
        onStartShouldSetPanResponder: () => true,
        onMoveShouldSetPanResponder: () => true,
        onPanResponderGrant: () => finger.on.setValue(1),
        onPanResponderRelease: (e, g) => {
          drag.setValue(Math.min(0, g.dy));
          finger.on.setValue(0);
          if (g.dy < -140 || g.vy < -0.9) {
            Animated.timing(drag, { toValue: -screen.height, duration: 260, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start(onUnlock);
          } else {
            Animated.spring(drag, { toValue: 0, useNativeDriver: true }).start();
          }
        },
      }),
    [drag, finger, onUnlock, screen.height],
  );
  const fade = up.interpolate({ inputRange: [-400, 0], outputRange: [0, 1], extrapolate: 'clamp' });
  return (
    <Animated.View {...pan.panHandlers} {...finger.handlers} style={[StyleSheet.absoluteFill, { opacity: Animated.multiply(v, fade), transform: [{ translateY: up }] }]}>
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
    <View pointerEvents="none" style={[styles.toasts, { top: screen.topBar + 10, left: (screen.width - 400) / 2 }]}>
      {toasts.map((n) => (
        <Toast key={n.id} n={n} t={t} />
      ))}
    </View>
  );
});

function Toast({ n, t }) {
  const v = useEntrance(true, 240);
  const ty = v.interpolate({ inputRange: [0, 1], outputRange: [-40, 0] });
  return (
    <Animated.View style={[styles.toast, { backgroundColor: t.panel, borderColor: t.panelLine, boxShadow: `0px 12px 32px ${t.shadow}`, opacity: v, transform: [{ translateY: ty }] }]}>
      {n.app ? <AppIcon app={n.app} size={28} /> : <Icon name={n.icon || 'bell'} size={22} color={t.accent} />}
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
  grid: { flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'center', paddingBottom: 12 },
  appTile: { width: 150, height: 156, borderRadius: 24, alignItems: 'center', justifyContent: 'flex-start', paddingTop: 14, paddingHorizontal: 8 },
  appName: { fontFamily: font, fontSize: 15, fontWeight: '500', marginTop: 10, height: 40, textAlign: 'center' },
  empty: { fontFamily: font, fontSize: 16, textAlign: 'center', width: '100%', marginTop: 40 },
  overviewHead: { position: 'absolute', left: 48, right: 36, height: 48, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  overviewTitle: { fontFamily: font, color: '#fff', fontSize: 28, fontWeight: '700', letterSpacing: -0.4 },
  textButton: { height: 48, borderRadius: 24, paddingHorizontal: 20, justifyContent: 'center' },
  textButtonText: { fontFamily: font, color: 'rgba(255,255,255,0.9)', fontSize: 16, fontWeight: '600' },
  headPill: { flexDirection: 'row', alignItems: 'center', height: 40, borderRadius: 20, paddingHorizontal: 14, marginLeft: 8 },
  headClose: { width: 40, height: 40, borderRadius: 20, alignItems: 'center', justifyContent: 'center', marginLeft: 8 },
  pill: { flexDirection: 'row', alignItems: 'center', height: 44, borderRadius: 22, paddingHorizontal: 18, marginTop: 12 },
  pillText: { fontFamily: font, color: '#fff', fontSize: 15, fontWeight: '600', marginLeft: 8 },
  cardHead: { height: 60, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 2, paddingBottom: 12 },
  cardTitle: { flex: 1, fontFamily: font, color: '#fff', fontSize: 17, fontWeight: '600', marginLeft: 10 },
  cardBody: { overflow: 'hidden', boxShadow: '0px 24px 60px rgba(0,0,0,0.55)' },
  cardFoot: { height: 64, flexDirection: 'row', alignItems: 'center', justifyContent: 'center' },
  overviewFoot: { position: 'absolute', left: 0, right: 0, alignItems: 'center' },
  thumbTitle: { height: 40, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 2 },
  thumbText: { flex: 1, fontFamily: font, fontSize: 15, fontWeight: '600', marginLeft: 10 },
  thumbClose: { width: 32, height: 32, borderRadius: 16, backgroundColor: 'rgba(255,255,255,0.18)', alignItems: 'center', justifyContent: 'center' },
  thumb: { borderRadius: 16, overflow: 'hidden', borderWidth: 3 },
  dropRight: { position: 'absolute', right: 12 },
  dropCenter: { position: 'absolute', left: 0, right: 0, alignItems: 'center' },
  qsHead: { flexDirection: 'row', alignItems: 'center', marginBottom: 14 },
  powerRow: { height: 60, borderRadius: 18, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 18, marginBottom: 8 },
  powerText: { fontFamily: font, fontSize: 16, fontWeight: '600', marginLeft: 14 },
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
  calCell: { width: 40, height: 40, margin: 2, borderRadius: 20, alignItems: 'center', justifyContent: 'center' },
  fadeBottom: { position: 'absolute', left: 0, right: 0, bottom: 0, height: 36 },
  calText: { fontFamily: font, fontSize: 14, fontWeight: '500' },
  lockBody: { position: 'absolute', left: 0, right: 0, top: '18%', alignItems: 'center' },
  lockTime: { fontFamily: font, color: '#fff', fontSize: 132, fontWeight: '700', letterSpacing: -4 },
  lockDate: { fontFamily: font, color: 'rgba(255,255,255,0.85)', fontSize: 26, fontWeight: '500' },
  lockNote: { flexDirection: 'row', alignItems: 'center', marginTop: 28, backgroundColor: 'rgba(255,255,255,0.14)', borderRadius: 22, height: 44, paddingHorizontal: 18 },
  lockNoteText: { fontFamily: font, color: '#fff', fontSize: 15, marginLeft: 10 },
  lockHandle: { position: 'absolute', left: 0, right: 0, bottom: 40, alignItems: 'center' },
  lockHint: { fontFamily: font, color: 'rgba(255,255,255,0.85)', fontSize: 17, marginTop: 4, fontWeight: '500' },
  toasts: { position: 'absolute', width: 400 },
  toast: { flexDirection: 'row', alignItems: 'center', borderRadius: 20, borderWidth: 1, padding: 16, marginBottom: 10 },
});
