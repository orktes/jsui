// One client window: the shell's frame (title bar with large touch buttons,
// shadow, rounded corners, resize borders) around the client's pixels, which
// the host keeps current in the host texture "win:<id>".
//
// The X window sits exactly under the content area, so presses on the content
// go to the client; the title bar and the borders are over the host's
// InputOnly frame window, so their presses come here. Dragging the title bar
// moves the window (wm:move on every move, drawn from the local drag position
// so it never lags); dragging it to the top edge maximizes, to a side edge
// snaps to that half (with a preview). Double-tap the title bar toggles
// maximize. Drag a border or corner grip to resize.
import React, { memo, useEffect, useRef, useState } from 'react';
import { Animated, Easing, HostTexture, PanResponder, Pressable, StyleSheet, Text, View } from 'react-native';
import { useAnchor } from './anchors.js';
import { AppIcon } from './AppIcon.jsx';
import { Icon } from './icons.jsx';
import { font, useTheme } from './theme.js';
import { appName, wm } from './wm.js';

const MIN_W = 180;
const MIN_H = 110;

function useLatest(value) {
  const ref = useRef(value);
  ref.current = value;
  return ref;
}

function TitleButton({ icon, onPress, danger, t, label, anchorName }) {
  const anchor = useAnchor(anchorName);
  return (
    <Pressable
      ref={anchor.ref}
      onLayout={anchor.onLayout}
      onPress={onPress}
      accessibilityLabel={label}
      hitSlop={4}
      style={({ pressed }) => [
        styles.titleButton,
        pressed && { backgroundColor: danger ? t.danger : t.panelLine },
      ]}
    >
      {({ pressed }) => <Icon name={icon} size={20} color={pressed && danger ? '#fff' : t.textDim} strokeWidth={2.2} />}
    </Pressable>
  );
}

export const Window = memo(function Window({ win, active, screen, apps, hidden, onPreview, onDragging, tablet, onMenu }) {
  const t = useTheme();
  const TB = screen.titleBar;
  const B = screen.border;
  const [drag, setDrag] = useState(null); // {x, y, w, h} while moving / resizing
  const latest = useLatest({ win, screen, drag, tablet });
  const appear = useRef(new Animated.Value(0)).current;
  const mini = useRef(new Animated.Value(win.minimized ? 1 : 0)).current;
  const lastTap = useRef(0);
  const titleAnchor = useAnchor(`win.${win.id}.title`);

  // Open: fade and grow in once the first frame of pixels is there.
  useEffect(() => {
    if (!win.ready) return;
    Animated.timing(appear, { toValue: 1, duration: 220, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, [win.ready, appear]);
  // Minimize: shrink towards the dock; restore: back.
  useEffect(() => {
    Animated.timing(mini, {
      toValue: win.minimized ? 1 : 0,
      duration: win.minimized ? 240 : 260,
      easing: Easing.out(Easing.cubic),
      useNativeDriver: true,
    }).start();
  }, [win.minimized, mini]);

  const move = useRef(null);
  if (!move.current) {
    let start = null;
    let moved = false;
    let preview = null;
    move.current = PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: () => {
        const { win: w } = latest.current;
        start = { x: w.x, y: w.y, w: w.w, h: w.h, max: w.maximized || !!w.snap, wasMax: w.maximized || !!w.snap, rw: w.restoreW, rh: w.restoreH };
        moved = false;
        preview = null;
        wm.focus(w.id);
      },
      onPanResponderMove: (e, g) => {
        const { win: w, screen: s } = latest.current;
        if (!moved && Math.abs(g.dx) + Math.abs(g.dy) < 8) return;
        if (!moved) {
          moved = true;
          onDragging(w.id, true);
          if (start.max) {
            // Leaving a maximized / snapped place: back to the old size, under the finger.
            const frac = Math.min(0.9, Math.max(0.1, (g.x0 - w.x) / w.w));
            // The finger stays on the title bar, at the same fraction of its width.
            start = { ...start, w: start.rw, h: start.rh, x: g.x0 - start.rw * frac, y: g.y0 + TB / 2 };
          }
        }
        const x = Math.round(start.x + g.dx);
        const y = Math.round(Math.max(s.topBar + TB, start.y + g.dy));
        setDrag({ x, y, w: start.w, h: start.h });
        wm.move(w.id, x, y);
        let p = null;
        if (w.resizable !== false && w.type === 'normal') {
          if (g.moveY <= s.topBar + 4) p = 'max';
          else if (g.moveX <= 6) p = 'left';
          else if (g.moveX >= s.width - 6) p = 'right';
        }
        if (p !== preview) {
          preview = p;
          onPreview(p);
        }
      },
      onPanResponderRelease: () => {
        const { win: w } = latest.current;
        if (!moved) {
          const now = Date.now();
          if (now - lastTap.current < 380 && w.type === 'normal') {
            wm.maximize(w.id, !w.maximized);
            lastTap.current = 0;
          } else {
            lastTap.current = now;
          }
        } else {
          if (preview === 'left' || preview === 'right') wm.snap(w.id, preview);
          // Tablet mode: a dragged app that lands nowhere goes back to full screen.
          else if (preview === 'max' || (latest.current.tablet && start.wasMax && w.type === 'normal')) wm.maximize(w.id, true);
          onDragging(w.id, false);
        }
        if (preview) onPreview(null);
        preview = null;
        setDrag(null);
      },
      onPanResponderTerminate: () => {
        onPreview(null);
        setDrag(null);
      },
    });
  }

  // Resize grips: edges = {left, right, bottom} flags.
  const grips = useRef({});
  const grip = (key, edges) => {
    if (!grips.current[key]) {
      let start = null;
      grips.current[key] = PanResponder.create({
        onStartShouldSetPanResponder: () => true,
        onMoveShouldSetPanResponder: () => true,
        onPanResponderTerminationRequest: () => false,
        onPanResponderGrant: () => {
          const { win: w } = latest.current;
          start = { x: w.x, y: w.y, w: w.w, h: w.h };
          wm.focus(w.id);
          onDragging(w.id, true);
        },
        onPanResponderMove: (e, g) => {
          const { win: w, screen: s } = latest.current;
          const minW = Math.max(MIN_W, w.minW || 0);
          const minH = Math.max(MIN_H, w.minH || 0);
          let { x, y, w: ww, h } = start;
          if (edges.right) ww = Math.max(minW, start.w + g.dx);
          if (edges.left) {
            ww = Math.max(minW, start.w - g.dx);
            x = start.x + start.w - ww;
          }
          if (edges.bottom) h = Math.max(minH, Math.min(s.height - y - 4, start.h + g.dy));
          const r = { x: Math.round(x), y: Math.round(y), w: Math.round(ww), h: Math.round(h) };
          setDrag(r);
          wm.resize(w.id, r.x, r.y, r.w, r.h);
        },
        onPanResponderRelease: () => {
          onDragging(latest.current.win.id, false);
          setDrag(null);
        },
        onPanResponderTerminate: () => setDrag(null),
      });
    }
    return grips.current[key].panHandlers;
  };

  const g = drag || win;
  const full = win.fullscreen;
  const decorated = win.decorated && !full;
  const max = win.maximized && !drag;
  const snapped = !!win.snap && !drag;
  const radius = full || max ? 0 : snapped ? 16 : 14;
  // Tablet mode, full screen or split: a slim header with a grabber, the app
  // name, a menu and close, instead of desktop window buttons.
  const tabletChrome = tablet && decorated && win.type === 'normal' && (max || snapped);
  const appEntry = apps.find((a) => a.id === win.appId);
  const menuAnchor = useAnchor(`win.${win.id}.menu`);
  const frameTop = decorated ? g.y - TB : g.y;
  const frameH = decorated ? g.h + TB : g.h;

  const opacity = Animated.multiply(appear, Animated.subtract(1, mini));
  const scale = appear.interpolate({ inputRange: [0, 1], outputRange: [0.94, 1] });
  const miniScale = mini.interpolate({ inputRange: [0, 1], outputRange: [1, 0.25] });
  const dropY = mini.interpolate({ inputRange: [0, 1], outputRange: [0, screen.height - frameTop - frameH / 2] });

  const shadow = active
    ? `0px 18px 48px ${t.shadow}, 0px 0px 0px 1px ${t.chromeLine}`
    : `0px 4px 12px ${t.shadow}, 0px 0px 0px 1px ${t.chromeLine}`;
  const resizable = decorated && !max && win.resizable !== false && !win.snap;

  return (
    <Animated.View
      pointerEvents={hidden || win.minimized ? 'none' : 'box-none'}
      style={[
        styles.frame,
        {
          left: g.x,
          top: frameTop,
          width: g.w,
          height: frameH,
          opacity: hidden ? 0 : opacity,
          transform: [{ translateY: dropY }, { scale: Animated.multiply(scale, miniScale) }],
        },
      ]}
    >
      <View style={[styles.surface, { borderRadius: radius, boxShadow: full ? undefined : shadow }]}>
        {decorated && tabletChrome && (
          <View
            {...move.current.panHandlers}
            ref={titleAnchor.ref}
            onLayout={titleAnchor.onLayout}
            style={[styles.titleBar, { height: TB, backgroundColor: t.chromeActive, borderTopLeftRadius: radius, borderTopRightRadius: radius }]}
          >
            <View style={[styles.tabletName, { opacity: active ? 1 : 0.6 }]}>
              <AppIcon win={win} app={appEntry} size={22} />
              <Text numberOfLines={1} style={[styles.tabletTitle, { color: active ? t.textDim : t.textMute }]}>{appName(win, apps)}</Text>
            </View>
            <View pointerEvents="none" style={styles.grabberWrap}>
              <View style={[styles.grabber, { backgroundColor: t.textMute }]} />
            </View>
            <View style={styles.tabletButtons}>
              <Pressable ref={menuAnchor.ref} onLayout={menuAnchor.onLayout} hitSlop={4}
                onPress={(e) => onMenu && onMenu(win, e.nativeEvent.pageX, g.y)}
                style={({ pressed }) => [styles.titleButton, pressed && { backgroundColor: t.panelLine }]}>
                <Icon name="more" size={22} color={t.textDim} strokeWidth={2.6} />
              </Pressable>
              <TitleButton t={t} icon="close" danger label="Close" anchorName={`win.${win.id}.close`} onPress={() => wm.close(win.id)} />
            </View>
          </View>
        )}
        {decorated && !tabletChrome && (
          <View
            {...move.current.panHandlers}
            ref={titleAnchor.ref}
            onLayout={titleAnchor.onLayout}
            style={[
              styles.titleBar,
              { height: TB, backgroundColor: active ? t.chromeActive : t.chrome, borderTopLeftRadius: radius, borderTopRightRadius: radius },
            ]}
          >
            <View style={[styles.titleIcon, { opacity: active ? 1 : 0.6 }]}>
              <AppIcon win={win} app={appEntry} size={26} />
            </View>
            <Text numberOfLines={1} style={[styles.title, { color: active ? t.text : t.textMute }]}>
              {win.title || appName(win, apps)}
            </Text>
            {win.type === 'normal' && <TitleButton t={t} icon="minimize" label="Minimize" anchorName={`win.${win.id}.minimize`} onPress={() => wm.minimize(win.id)} />}
            {win.type === 'normal' && win.resizable !== false && !snapped && (
              <TitleButton t={t} icon={win.maximized ? 'restore' : 'maximize'} label="Maximize" anchorName={`win.${win.id}.maximize`}
                onPress={() => wm.maximize(win.id, !win.maximized)} />
            )}
            <TitleButton t={t} icon="close" danger label="Close" anchorName={`win.${win.id}.close`} onPress={() => wm.close(win.id)} />
          </View>
        )}
        <View
          style={{
            width: g.w,
            height: g.h,
            overflow: 'hidden',
            backgroundColor: t.chrome,
            borderBottomLeftRadius: radius,
            borderBottomRightRadius: radius,
            borderTopLeftRadius: decorated ? 0 : radius,
            borderTopRightRadius: decorated ? 0 : radius,
          }}
        >
          <HostTexture name={`win:${win.id}`} style={{ width: g.w, height: g.h }} resizeMode="stretch" />
        </View>
      </View>
      {resizable && (
        <>
          <View {...grip('l', { left: true })} style={[styles.edge, { left: -B, top: TB, width: B, bottom: B }]} />
          <View {...grip('r', { right: true })} style={[styles.edge, { right: -B, top: TB, width: B, bottom: B }]} />
          <View {...grip('b', { bottom: true })} style={[styles.edge, { left: B, right: B, bottom: -B, height: B }]} />
          <View {...grip('bl', { left: true, bottom: true })} style={[styles.edge, { left: -B, bottom: -B, width: B * 2, height: B * 2 }]} />
          <View {...grip('br', { right: true, bottom: true })} style={[styles.edge, { right: -B, bottom: -B, width: B * 3, height: B * 3 }]}>
            {active && <View pointerEvents="none" style={[styles.gripMark, { right: B + 3, bottom: B + 3, borderColor: t.textMute }]} />}
          </View>
        </>
      )}
    </Animated.View>
  );
});

// A window that just closed: its last frame fades and shrinks, then the host
// may drop the texture.
export const ClosingWindow = memo(function ClosingWindow({ win, screen, onDone }) {
  const t = useTheme();
  const v = useRef(new Animated.Value(1)).current;
  useEffect(() => {
    Animated.timing(v, { toValue: 0, duration: 180, easing: Easing.in(Easing.quad), useNativeDriver: true }).start(() => onDone(win.id));
  }, [v, win.id, onDone]);
  const TB = win.decorated && !win.fullscreen ? screen.titleBar : 0;
  const scale = v.interpolate({ inputRange: [0, 1], outputRange: [0.92, 1] });
  return (
    <Animated.View
      pointerEvents="none"
      style={[styles.frame, { left: win.x, top: win.y - TB, width: win.w, height: win.h + TB, opacity: v, transform: [{ scale }] }]}
    >
      <View style={[styles.surface, { borderRadius: 14, boxShadow: `0px 12px 32px ${t.shadow}` }]}>
        {TB > 0 && <View style={{ height: TB, backgroundColor: t.chrome }} />}
        <HostTexture name={`win:${win.id}`} style={{ width: win.w, height: win.h }} resizeMode="stretch" />
      </View>
    </Animated.View>
  );
});

const styles = StyleSheet.create({
  frame: { position: 'absolute' },
  surface: { flex: 1, overflow: 'hidden' },
  titleBar: { flexDirection: 'row', alignItems: 'center', paddingLeft: 12, paddingRight: 4 },
  titleIcon: { width: 30, alignItems: 'center', marginRight: 8 },
  title: { flex: 1, fontFamily: font, fontSize: 16, fontWeight: '600' },
  titleButton: { width: 48, height: 38, borderRadius: 12, alignItems: 'center', justifyContent: 'center', marginLeft: 2 },
  edge: { position: 'absolute' },
  gripMark: {
    position: 'absolute',
    width: 18,
    height: 18,
    borderRightWidth: 3,
    borderBottomWidth: 3,
    borderBottomRightRadius: 12,
    opacity: 0.8,
  },
  tabletName: { flex: 1, flexDirection: 'row', alignItems: 'center' },
  tabletTitle: { fontFamily: font, fontSize: 14, fontWeight: '600', marginLeft: 10, flexShrink: 1 },
  grabberWrap: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center' },
  grabber: { width: 44, height: 6, borderRadius: 3, opacity: 0.7 },
  tabletButtons: { flex: 1, flexDirection: 'row', justifyContent: 'flex-end' },
});
