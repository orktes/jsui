// Full-screen and drop-down panels: the app launcher, the window overview,
// quick settings, the calendar / notifications panel and the lock screen.
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  Animated, Easing, HostTexture, PanResponder, Pressable, ScrollView, StyleSheet, Text, TextInput, View, useHostMessage,
} from 'react-native';
import { remeasureAnchors, useAnchor } from './anchors.js';
import { useNativeDragY } from './drag.js';
import { AppIcon } from './AppIcon.jsx';
import { dateText, longMonth, pad2, useClock } from './bars.jsx';
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

const AppTile = memo(function AppTile({ app, width, onLaunch }) {
  const t = useTheme();
  const anchor = useAnchor(`app.${app.id}`);
  return (
    <Pressable
      ref={anchor.ref}
      onLayout={anchor.onLayout}
      onPress={() => onLaunch(app)}
      style={[styles.appTile, { width }]} pressedStyle={{ backgroundColor: 'rgba(255,255,255,0.12)', transform: [{ scale: 0.95 }] }}
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

export const Launcher = memo(function Launcher({ screen, apps, onClose, onClosed, onLaunch, settings, open = true, warm = false }) {
  const t = useTheme();
  const [query, setQuery] = useState('');
  // Kept mounted between opens (shell.jsx): start afresh, once it is gone
  // (a change while it fades would paint the grid again).
  const closed = useCallback(() => {
    setQuery('');
    onClosed && onClosed();
  }, [onClosed]);
  const v = useEntrance(open, 200, closed);
  const pop = useSpringIn(open);
  // Painted once at idle after start (shell.jsx `warm`), at an opacity that
  // changes no pixel and with touches passing through: the first open then
  // finds the icons uploaded, the glyphs rasterized and the grid's layer
  // cached, instead of all of it in its first frame (~75 ms on the Tab A7).
  // A few frames: the grid gets its layer once it is unchanged for one.
  useEffect(() => {
    if (!warm || open) return undefined;
    v.setValue(0.002);
    const a = Animated.parallel([
      Animated.timing(v, { toValue: 0.003, duration: 250, useNativeDriver: true }),
      Animated.sequence([
        Animated.timing(pop, { toValue: 0.3, duration: 120, useNativeDriver: true }),
        Animated.timing(pop, { toValue: 0, duration: 120, useNativeDriver: true }),
      ]),
    ]);
    a.start(({ finished }) => finished && v.setValue(0));
    return () => a.stop();
  }, [warm]); // eslint-disable-line react-hooks/exhaustive-deps
  const input = useRef(null);
  useEffect(() => {
    if (!open) return undefined;
    wm.keyboard(true); // the search field gets the physical keyboard
    // Focused once the fade is done: a blinking caret during it would keep
    // the grid's cached layer stale (every frame painted in full).
    const id = setTimeout(() => input.current && input.current.focus && input.current.focus(), 230);
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
  // Stable, so opening and closing (a new `open`) doesn't render every
  // AppTile again: ~30 of them cost the JS thread ~15 ms per open on the
  // Tab A7.
  const launch = useCallback((app) => {
    onLaunch(app);
    onClose();
  }, [onLaunch, onClose]);
  const sc = pop.interpolate({ inputRange: [0, 1], outputRange: [0.96, 1] });
  // As many 150 px columns as fit with 24 px margins, then as few as give the
  // same number of rows (18 apps: 6 x 3, not 7 + 7 + 4). When that many rows
  // don't fit the height (portrait), narrower columns (down to 120 px) until
  // every app shows without scrolling. The search field spans the grid.
  const bottom = settings.osk ? 330 : screen.dock + 12;
  // (A tile ends in its second label line and padding: the last row may
  // reach 40 px into the gap above the dock.)
  const rowsFit = Math.max(1, Math.floor((screen.height - screen.topBar - 28 - 64 - 28 - bottom + 40) / 156));
  const fit = Math.max(3, Math.min(8, Math.floor((screen.width - 48) / 150)));
  let cols = Math.min(fit, Math.max(3, Math.ceil(apps.length / Math.ceil(apps.length / fit))));
  let tileW = 150;
  if (Math.ceil(apps.length / cols) > rowsFit) {
    cols = Math.min(Math.floor((screen.width - 48) / 120), Math.ceil(apps.length / rowsFit));
    tileW = Math.min(150, Math.floor((screen.width - 48) / cols));
  }
  return (
    <View pointerEvents="box-none" style={StyleSheet.absoluteFill}>
      <Under screen={screen} />
      {/* The scrim fades on its own: its blurred backdrop is a new snapshot
          at every open, and inside the grid's layer it made that layer
          stale every time (the whole grid painted again). */}
      <Animated.View pointerEvents={open ? 'auto' : 'none'} style={[StyleSheet.absoluteFill, { opacity: v }]}>
        <Scrim color="rgba(6,8,20,0.82)" onPress={onClose} screen={screen} />
      </Animated.View>
      {/* The search field and the grid fade as layers of their own: the
          field's caret and focus change at every open, and inside the grid's
          layer they made it stale (the whole grid painted again). */}
      <View pointerEvents={open ? 'box-none' : 'none'} style={StyleSheet.absoluteFill}>
        <Animated.View pointerEvents="box-none" style={[styles.launcher, { top: screen.topBar + 28, bottom, transform: [{ scale: sc }] }]}>
          <Animated.View style={{ opacity: v }}>
            <View style={[styles.search, { width: cols * tileW - 32, backgroundColor: t.panel, borderColor: t.panelLine, boxShadow: `0px 12px 36px ${t.shadow}` }]}>
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
          </Animated.View>
          <Animated.View style={{ flex: 1, marginTop: 28, width: cols * tileW, opacity: v }}>
            {query ? <Label style={{ marginBottom: 8, marginLeft: 10, color: 'rgba(255,255,255,0.7)' }}>{`${list.length} result${list.length === 1 ? '' : 's'}`}</Label> : null}
            <ScrollView contentContainerStyle={styles.grid} showsVerticalScrollIndicator={false}>
              {list.map((app) => (
                <AppTile key={app.id} app={app} width={tileW} onLaunch={launch} />
              ))}
              {!list.length && <Text style={[styles.empty, { color: 'rgba(255,255,255,0.6)' }]}>No apps match “{query}”</Text>}
            </ScrollView>
          </Animated.View>
        </Animated.View>
      </View>
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

// Wi-Fi networks in range (Quick Settings' Wi-Fi chevron): tap one to join;
// a secured network NetworkManager has no password for asks for one first
// (with the on-screen keyboard up, on touch screens).
function WifiNetworks({ settings, onBack }) {
  const t = useTheme();
  const [nets, setNets] = useState(null);
  const [scans, setScans] = useState(0); // answers so far (a cached list, then a fresh scan)
  const [ask, setAsk] = useState(null); // the network a password is being typed for
  const [password, setPassword] = useState('');
  const [joining, setJoining] = useState(null);
  const [error, setError] = useState(null);
  const raisedOsk = useRef(false);
  const osk = useRef(settings.osk);
  osk.current = settings.osk;
  useHostMessage('wm:wifiNetworks', (m) => {
    setNets(m.networks || []);
    setScans((n) => n + 1);
  });
  const lastJoin = useRef(null); // { net, typed }
  useHostMessage('wm:wifiResult', (m) => {
    setJoining(null);
    if (m.ok) {
      setAsk(null);
      setPassword('');
      setError(null);
      lowerOsk();
      return;
    }
    const last = lastJoin.current;
    const msg = m.message || '';
    // NetworkManager says "Secrets were required" for a missing password and
    // for a wrong one alike.
    setError(/secrets were required|802-1x|psk/i.test(msg) ? (last && last.typed ? 'Wrong password' : 'Password needed') : msg || 'Could not join');
    // A saved secured network without a (working) password: ask for one.
    if (last && !last.typed && last.net.ssid === m.ssid && last.net.secure) askFor(last.net);
  });
  useEffect(() => {
    wm.wifiScan(false);
    wm.wifiScan(true);
    return lowerOsk;
  }, []);
  // Keys (the on-screen keyboard's too) go to the shell while the password
  // field is up, not to the app under it.
  const asking = !!ask;
  useEffect(() => {
    if (!asking) return undefined;
    wm.keyboard(true);
    return () => wm.keyboard(false);
  }, [asking]);
  function lowerOsk() {
    if (raisedOsk.current && osk.current) setSetting('osk', false);
    raisedOsk.current = false;
  }
  const join = (n, pw = '') => {
    lastJoin.current = { net: n, typed: !!pw };
    setError(null);
    setJoining(n.ssid);
    wm.wifiConnect(n.ssid, pw);
  };
  const pick = (n) => {
    if (n.active || joining) return;
    if (n.secure && !n.known) {
      setError(null);
      askFor(n);
    } else {
      join(n);
    }
  };
  function askFor(n) {
    setAsk(n);
    setPassword('');
    if (!osk.current) {
      raisedOsk.current = true;
      setSetting('osk', true);
    }
  }
  const head = (title, back, right) => (
    <View style={styles.wifiHead}>
      <IconButton icon="left" size={44} iconSize={22} onPress={back} label="Back" />
      <Text numberOfLines={1} style={[styles.wifiTitle, { color: t.text }]}>{title}</Text>
      {right}
    </View>
  );
  if (ask) {
    return (
      <View style={{ marginBottom: 6 }}>
        {head(ask.ssid, () => { setAsk(null); setError(null); lowerOsk(); })}
        <TextInput
          value={password}
          onChangeText={setPassword}
          secureTextEntry
          autoFocus
          placeholder="Password"
          placeholderTextColor={t.textMute}
          onSubmitEditing={() => password && join(ask, password)}
          style={[styles.wifiInput, { color: t.text, backgroundColor: t.panelRaised, borderColor: t.panelLine }]}
        />
        {error ? <Text style={[styles.wifiNote, { color: '#ff6b6b' }]}>{error}</Text> : null}
        <Pressable onPress={() => password && !joining && join(ask, password)}
          style={[styles.powerRow, { backgroundColor: password ? t.accent : t.panelRaised, justifyContent: 'center', marginTop: 10 }]}
          pressedStyle={{ opacity: 0.8 }}>
          <Text style={[styles.powerText, { marginLeft: 0, color: password ? t.accentText : t.textMute }]}>
            {joining ? 'Joining…' : 'Join'}
          </Text>
        </Pressable>
      </View>
    );
  }
  return (
    <View style={{ marginBottom: 6 }}>
      {head('Wi-Fi', onBack,
        <IconButton icon="refresh" size={44} iconSize={20} onPress={() => { setScans(1); wm.wifiScan(true); }} label="Scan" />)}
      {error ? <Text style={[styles.wifiNote, { color: '#ff6b6b' }]}>{error}</Text> : null}
      {nets == null || (scans < 2 && !nets.length) ? (
        <Text style={[styles.wifiNote, { color: t.textMute }]}>Searching for networks…</Text>
      ) : !nets.length ? (
        <Text style={[styles.wifiNote, { color: t.textMute }]}>No networks found.</Text>
      ) : (
        <ScrollView style={{ maxHeight: 340 }} showsVerticalScrollIndicator={false}>
          {nets.map((n) => (
            <Pressable key={n.ssid} onPress={() => pick(n)}
              style={[styles.wifiRow, { backgroundColor: n.active ? t.tileOn : t.panelRaised }]} pressedStyle={{ backgroundColor: t.panelLine }}>
              <View style={{ opacity: 0.35 + 0.65 * Math.min(1, n.signal / 80) }}>
                <Icon name="wifi" size={22} color={n.active ? t.accentText : t.text} />
              </View>
              <View style={{ flex: 1, marginLeft: 14 }}>
                <Text numberOfLines={1} style={[styles.wifiName, { color: n.active ? t.accentText : t.text }]}>{n.ssid}</Text>
                <Text numberOfLines={1} style={[styles.tileSubText, { color: n.active ? 'rgba(255,255,255,0.8)' : t.textMute }]}>
                  {joining === n.ssid ? 'Joining…' : n.active ? 'Connected' : n.known ? 'Saved' : n.secure ? 'Secured' : 'Open'}
                </Text>
              </View>
              {n.secure && <Icon name="lock" size={18} color={n.active ? t.accentText : t.textDim} />}
            </Pressable>
          ))}
        </ScrollView>
      )}
    </View>
  );
}


export const QuickSettings = memo(function QuickSettings({ screen, settings, system, windows, onClose, onClosed, onLock, onReload, open = true }) {
  const t = useTheme();
  const v = useEntrance(open, 200, onClosed);
  const [power, setPower] = useState(false);
  const [wifiOpen, setWifiOpen] = useState(false);
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
          {wifiOpen && !power && <WifiNetworks settings={settings} onBack={() => setWifiOpen(false)} />}
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
          {!power && !wifiOpen && <View style={styles.tiles}>
            <ToggleTile anchor={a.wifi} icon="wifi" label="Wi-Fi"
              sub={system.wifi === null ? 'Not available' : !settings.wifi ? 'Off' : system.wifi && system.wifi.up ? 'Connected' : 'On'}
              on={settings.wifi && system.wifi !== null}
              onPress={() => {
                setSetting('wifi', !settings.wifi);
                if (system.wifi) wm.wifiRadio(!settings.wifi);
              }}
              onMore={system.wifi ? () => { setPower(false); setWifiOpen(true); } : undefined} />
            {system.rotate ? (
              <ToggleTile icon="rotate" label="Auto-rotate" sub={settings.autoRotate ? 'On' : 'Off'} on={settings.autoRotate} onPress={set('autoRotate')} />
            ) : (
              <ToggleTile icon="bluetooth" label="Bluetooth" on={settings.bluetooth} onPress={set('bluetooth')} />
            )}
            <ToggleTile anchor={a.dark} icon="moon" label="Dark style" on={settings.darkMode} onPress={set('darkMode')} />
            <ToggleTile anchor={a.tablet} icon="tablet" label="Tablet mode" sub={settings.tabletMode ? 'Apps open full' : 'Floating windows'} on={settings.tabletMode} onPress={() => {
              const on = !settings.tabletMode;
              setSetting('tabletMode', on);
              if (on) for (const w of windows) if (w.type === 'normal' && !w.transientFor && !w.minimized && !w.snap) wm.maximize(w.id, true);
            }} />
            <ToggleTile anchor={a.night} icon="sun" label="Night light" on={settings.nightLight} onPress={set('nightLight')} />
            <ToggleTile icon="bell" label="Do not disturb" on={settings.doNotDisturb} onPress={set('doNotDisturb')} />
          </View>}
          {/* Hosts with a backlight and a sound server (system.backlight,
              system.volume) set the real ones; elsewhere brightness dims the
              shell's picture (shell.jsx) and volume is only the setting. */}
          {!power && !wifiOpen && <Slider anchor={a.brightness} icon="sun" value={settings.brightness} onChange={(x) => {
            const b = Math.max(system.backlight != null ? 0.04 : 0.2, x);
            setSetting('brightness', b);
            if (system.backlight != null) wm.brightness(b);
          }} />}
          {!power && !wifiOpen && <Slider icon={settings.muted || settings.volume === 0 ? 'mute' : 'volume'} value={settings.volume} onChange={(x) => {
            setSetting('volume', x);
            if (settings.muted) setSetting('muted', false);
            if (system.volume) wm.volume(x, false);
          }} />}
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

export const CalendarPanel = memo(function CalendarPanel({ screen, notifications, onClear, onDismiss, onOpen, onClose, onClosed, open = true }) {
  const t = useTheme();
  const now = useClock();
  const v = useEntrance(open, 200, onClosed);
  const ty = v.interpolate({ inputRange: [0, 1], outputRange: [-24, 0] });
  const cells = monthGrid(now);
  const narrow = screen.width < 760;
  return (
    <View pointerEvents={open ? 'auto' : 'none'} style={StyleSheet.absoluteFill}>
      <Animated.View style={[StyleSheet.absoluteFill, { opacity: v }]}>
        <Pressable style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(0,0,0,0.25)' }]} onPress={onClose} />
      </Animated.View>
      <Animated.View style={[styles.dropCenter, { top: screen.topBar + 10, opacity: v, transform: [{ translateY: ty }] }]}>
        {/* Side by side; on a narrow (portrait) screen the calendar goes on
            top of the notifications, with the same 12 px margins as quick
            settings. The status bar right above shows the time. */}
        <Card style={narrow ? { width: screen.width - 24, flexDirection: 'column-reverse' } : { width: 720, flexDirection: 'row' }}>
          <View style={narrow ? { paddingTop: 16 } : { flex: 1, paddingRight: 18 }}>
            <View style={styles.notesHead}>
              <Label style={{ flex: 1 }}>Notifications</Label>
              {notifications.length > 0 && (
                <Pressable onPress={onClear} hitSlop={8} style={styles.clearAll} pressedStyle={{ backgroundColor: t.panelLine }}>
                  <Text style={[styles.clearAllText, { color: t.accent }]}>Clear all</Text>
                </Pressable>
              )}
            </View>
            {notifications.length === 0 && <Text style={[styles.empty, { color: t.textMute, marginTop: 30, marginBottom: 30 }]}>You're all caught up</Text>}
            <View style={{ maxHeight: 340, marginTop: 8 }}>
            <ScrollView style={{ maxHeight: 340 }} contentContainerStyle={{ paddingBottom: notifications.length > 3 ? 24 : 0 }}>
              {notifications.map((n) => (
                <Pressable key={n.id} style={[styles.note, { backgroundColor: t.panelRaised }]} pressedStyle={{ backgroundColor: t.panelLine }}
                  onPress={() => {
                    // An app's notification opens the app (and the panel goes).
                    if (!onOpen || (!n.app && n.notifyId == null)) return;
                    onOpen(n);
                    onClose();
                  }}>
                  {n.app ? <AppIcon app={n.app} size={32} /> : <Icon name={n.icon || 'bell'} size={22} color={t.accent} />}
                  <View style={{ flex: 1, marginLeft: 12 }}>
                    <Text numberOfLines={2} style={[styles.noteTitle, { color: t.text }]}>{n.title}</Text>
                    {n.body ? <Text numberOfLines={2} style={[styles.noteBody, { color: t.textDim }]}>{n.body}</Text> : null}
                  </View>
                  <Text style={[styles.noteTime, { color: t.textMute }]}>{n.time}</Text>
                  <Pressable onPress={() => onDismiss(n.id)} hitSlop={10} style={styles.noteClose} pressedStyle={{ backgroundColor: t.panelLine }}>
                    <Icon name="close" size={16} color={t.textMute} />
                  </Pressable>
                </Pressable>
              ))}
            </ScrollView>
            {/* Only when the list can scroll: over a short list it showed as an empty card. */}
            {notifications.length > 3 && (
              <View pointerEvents="none" style={[styles.fadeBottom, { experimental_backgroundImage: `linear-gradient(180deg, rgba(0,0,0,0) 0%, ${t.panel} 100%)` }]} />
            )}
            </View>
          </View>
          <View style={narrow ? { height: 1, backgroundColor: t.panelLine } : { width: 1, backgroundColor: t.panelLine }} />
          <View style={narrow ? { alignItems: 'center', paddingBottom: 16 } : { width: 320, paddingLeft: 18 }}>
            <Text style={[styles.calTitle, { color: t.text }, narrow && { width: 300 }]}>{longDate(now)}</Text>
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

// "Saturday 7 November": the calendar's heading.
const longDate = (d) =>
  `${['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'][d.getDay()]} ${d.getDate()} ${longMonth(d)}`;

// --------------------------------------------------------------- Lock screen

// `children` is its background (the wallpaper): opaque, so neither the apps
// nor the desktop's own clock show through while it is up.
export const LockScreen = memo(function LockScreen({ screen, onUnlock, notifications, children }) {
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
      {children}
      <View style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(6,8,22,0.35)' }]} />
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

export const Toasts = memo(function Toasts({ screen, toasts, onDone, onOpen }) {
  const t = useTheme();
  return (
    <View pointerEvents="box-none" style={[styles.toasts, { top: screen.topBar + 10, left: (screen.width - 400) / 2 }]}>
      {toasts.map((n) => (
        <Toast key={n.id} n={n} t={t} onDone={onDone} onOpen={onOpen} />
      ))}
    </View>
  );
});

// Shown for a few seconds, then slides back up; a tap puts it away sooner.
function Toast({ n, t, onDone, onOpen }) {
  const [open, setOpen] = useState(true);
  const v = useEntrance(open, 240, () => onDone(n.id));
  useEffect(() => {
    const id = setTimeout(() => setOpen(false), 3200);
    return () => clearTimeout(id);
  }, []);
  const ty = v.interpolate({ inputRange: [0, 1], outputRange: [-40, 0] });
  return (
    <Animated.View style={{ opacity: v, transform: [{ translateY: ty }] }}>
      {/* A tap puts it away; an app's notification also opens the app (onOpen). */}
      <Pressable onPress={() => { setOpen(false); if ((n.app || n.notifyId != null) && onOpen) onOpen(n); }} style={[styles.toast, { backgroundColor: t.panel, borderColor: t.panelLine, boxShadow: `0px 12px 32px ${t.shadow}` }]}>
        {n.app ? <AppIcon app={n.app} size={28} /> : <Icon name={n.icon || 'bell'} size={22} color={t.accent} />}
        <View style={{ flex: 1, marginLeft: 12 }}>
          <Text numberOfLines={1} style={[styles.noteTitle, { color: t.text }]}>{n.title}</Text>
          {n.body ? <Text numberOfLines={2} style={[styles.noteBody, { color: t.textDim }]}>{n.body}</Text> : null}
        </View>
      </Pressable>
    </Animated.View>
  );
}

const styles = StyleSheet.create({
  launcher: { position: 'absolute', left: 0, right: 0, alignItems: 'center' },
  search: { height: 64, borderRadius: 32, borderWidth: 1, flexDirection: 'row', alignItems: 'center', paddingLeft: 22, paddingRight: 8 },
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
  // No shadow: over the dark scrim it barely showed, and a 60 px blur under a
  // card the size of the screen cost the Tab A7's GPU ~6 ms a frame.
  cardBody: { overflow: 'hidden' },
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
  wifiHead: { flexDirection: 'row', alignItems: 'center', marginBottom: 8, marginLeft: -6 },
  wifiTitle: { flex: 1, fontFamily: font, fontSize: 18, fontWeight: '700', marginLeft: 4 },
  wifiRow: { height: 62, borderRadius: 18, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 18, marginBottom: 8 },
  wifiName: { fontFamily: font, fontSize: 16, fontWeight: '600' },
  tileSubText: { fontFamily: font, fontSize: 13, marginTop: 1 },
  wifiInput: { height: 54, borderRadius: 16, borderWidth: 1, paddingHorizontal: 16, fontFamily: font, fontSize: 17 },
  wifiNote: { fontFamily: font, fontSize: 14, marginVertical: 10, marginHorizontal: 6 },
  avatar: { width: 48, height: 48, borderRadius: 24, alignItems: 'center', justifyContent: 'center' },
  qsUser: { fontFamily: font, fontSize: 17, fontWeight: '700' },
  qsHost: { fontFamily: font, fontSize: 13, marginTop: 2 },
  tiles: { flexDirection: 'row', flexWrap: 'wrap', gap: 10, marginBottom: 10 },
  note: { flexDirection: 'row', alignItems: 'center', borderRadius: 18, padding: 14, marginBottom: 8 },
  noteTitle: { fontFamily: font, fontSize: 15, fontWeight: '600' },
  noteBody: { fontFamily: font, fontSize: 13, marginTop: 2 },
  noteTime: { fontFamily: font, fontSize: 12, marginLeft: 8 },
  noteClose: { width: 32, height: 32, borderRadius: 16, alignItems: 'center', justifyContent: 'center', marginLeft: 6, marginRight: -6 },
  calTitle: { fontFamily: font, fontSize: 18, fontWeight: '700', marginBottom: 12, marginLeft: 4 },
  notesHead: { flexDirection: 'row', alignItems: 'center', height: 32 },
  clearAll: { height: 32, borderRadius: 16, paddingHorizontal: 12, justifyContent: 'center', marginRight: -8 },
  clearAllText: { fontFamily: font, fontSize: 14, fontWeight: '600' },
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
