// One client window: the shell's frame (title bar with large touch buttons,
// shadow, rounded corners, resize borders) around the client's pixels, which
// the host keeps current in the host texture "win:<id>".
//
// The X window sits exactly under the content area, so presses on the content
// go to the client; the title bar and the borders are over the host's
// InputOnly frame window, so their presses come here. Dragging the title bar
// moves the window: the frame follows the finger natively (useNativeDragXY,
// no JS on a move), the host hears wm:move a few times a second and where
// it lands; dragging it to the top edge maximizes, to a side edge snaps to
// that half (with a preview the shell draws from the same native finger). Double-tap the title bar toggles
// maximize. Drag a border or corner grip to resize.
import React, { memo, useEffect, useRef, useState } from 'react';
import { Animated, Easing, HostTexture, PanResponder, Pressable, StyleSheet, Text, View } from 'react-native';
import { anchorRect, takeLaunchOrigin, useAnchor } from './anchors.js';
import { useNativeDragXY } from './drag.js';
import { AppIcon } from './AppIcon.jsx';
import { Icon } from './icons.jsx';
import { font, useTheme } from './theme.js';
import { appKeyOf, appName, windowTitle, wm } from './wm.js';
import { WindowMenu } from './WindowMenu.jsx';

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
      style={styles.titleButton}
      pressedStyle={{ backgroundColor: danger ? t.danger : t.panelLine }}
    >
      {({ pressed }) => <Icon name={icon} size={20} color={pressed && danger ? '#fff' : t.textDim} strokeWidth={2.2} />}
    </Pressable>
  );
}

export const Window = memo(function Window({ win, active, screen, apps, hidden, onPreview, onDragging, tablet, onMenu, menuOpen, onMenuClose, closing, onClosed, splashed }) {
  const t = useTheme();
  const TB = screen.titleBar;
  const B = screen.border;
  const [drag, setDrag] = useState(null); // {x, y, w, h} while moving / resizing
  const latest = useLatest({ win, screen, drag, tablet });
  const appear = useRef(new Animated.Value(0)).current;
  const mini = useRef(new Animated.Value(win.minimized ? 1 : 0)).current;
  const lastTap = useRef(0);
  const titleAnchor = useAnchor(`win.${win.id}.title`);
  const finger = useNativeDragXY();

  // Closed (the app went away; the shell keeps this mounted with its last
  // frame): into its dock icon when the dock has one, else a quick fade and
  // shrink, then onClosed (the host may drop the texture).
  const out = useRef(new Animated.Value(0)).current;
  const target = useRef(undefined);
  if (closing && target.current === undefined) target.current = win.transientFor ? null : anchorRect(`dock.${appKeyOf(win)}`) || null;
  useEffect(() => {
    if (!closing) return;
    const d = target.current;
    Animated.timing(out, { toValue: 1, duration: d ? 300 : 180, easing: d ? Easing.inOut(Easing.cubic) : Easing.in(Easing.quad), useNativeDriver: true })
      .start(() => onClosed && onClosed(win.id));
  }, [closing]); // eslint-disable-line react-hooks/exhaustive-deps

  // Open: fade and grow in once the first frame of pixels is there; out of
  // the icon it was launched from, when there is one (launcher, dock).
  const origin = useRef(undefined);
  if (origin.current === undefined) origin.current = win.transientFor ? null : takeLaunchOrigin(win.appId);
  // Under its app's loading window (splashed: LaunchSplash in shell.jsx) it is
  // simply there: that fades away over it.
  const instant = useRef(splashed);
  useEffect(() => {
    if (!win.ready) return;
    if (instant.current) return appear.setValue(1);
    Animated.timing(appear, {
      toValue: 1,
      duration: origin.current ? 340 : 220,
      easing: Easing.out(Easing.cubic),
      useNativeDriver: true,
    }).start();
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
    let lastMove = 0;
    let last = null; // the latest place, sent on release
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
          // From here the frame is at `start` plus the finger's travel, natively.
          setDrag({ x: start.x, y: start.y, w: start.w, h: start.h, minDy: s.topBar + TB - start.y });
          finger.on.setValue(1);
          if (w.resizable !== false && w.type === 'normal') onPreview({ x: finger.x, y: finger.y });
        }
        const x = Math.round(start.x + g.dx);
        const y = Math.round(Math.max(s.topBar + TB, start.y + g.dy));
        // The host only needs the window's place for input and its popups:
        // a few times a second while it moves (each one re-places the client
        // surface), and exactly where it lands (release).
        const now = Date.now();
        if (now - lastMove >= 100) {
          lastMove = now;
          wm.move(w.id, x, y);
        }
        last = { x, y };
        let p = null;
        if (w.resizable !== false && w.type === 'normal') {
          if (g.moveY <= s.topBar + 4) p = 'max';
          else if (g.moveX <= 6) p = 'left';
          else if (g.moveX >= s.width - 6) p = 'right';
        }
        preview = p;
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
          if (last) wm.move(w.id, last.x, last.y);
          if (preview) wm.haptic();
          if (preview === 'left' || preview === 'right') wm.snap(w.id, preview);
          // Tablet mode: a dragged app that lands nowhere goes back to full screen.
          else if (preview === 'max' || (latest.current.tablet && start.wasMax && w.type === 'normal')) wm.maximize(w.id, true);
          else if (last) {
            // Stays where it was let go (no native offset now) until the
            // host's place for it catches up.
            setDrag({ x: last.x, y: last.y, w: start.w, h: start.h, landing: true });
            last = null;
            finger.on.setValue(0);
            onPreview(null);
            onDragging(w.id, false);
            preview = null;
            return;
          }
          last = null;
          onDragging(w.id, false);
        }
        finger.on.setValue(0);
        onPreview(null);
        preview = null;
        setDrag(null);
      },
      onPanResponderTerminate: () => {
        finger.on.setValue(0);
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

  // Landed: back to the host's place once it has it (or soon anyway).
  useEffect(() => {
    if (!drag || !drag.landing) return undefined;
    if (win.x === drag.x && win.y === drag.y) {
      setDrag(null);
      return undefined;
    }
    const id = setTimeout(() => setDrag(null), 400);
    return () => clearTimeout(id);
  }, [drag, win.x, win.y]);
  const moving = drag && drag.minDy != null;
  const dragX = moving ? finger.dx : 0;
  const dragY = moving
    ? finger.dy.interpolate({ inputRange: [drag.minDy, drag.minDy + 1], outputRange: [drag.minDy, drag.minDy + 1], extrapolateLeft: 'clamp', extrapolateRight: 'extend' })
    : 0;

  const g = drag || win;
  const full = win.fullscreen;
  const decorated = win.decorated && !full;
  const max = win.maximized && !drag;
  const snapped = !!win.snap && !drag;
  const radius = full || max ? 0 : snapped ? 16 : 14;
  // Tablet mode, full screen or split: a slim header with a grabber, the app
  // name and a menu (which has Close), instead of desktop window buttons.
  const tabletChrome = tablet && decorated && win.type === 'normal' && (max || snapped);
  const appEntry = apps.find((a) => a.id === win.appId);
  const menuAnchor = useAnchor(`win.${win.id}.menu`);
  const frameTop = decorated ? g.y - TB : g.y;
  const frameH = decorated ? g.h + TB : g.h;

  const o = origin.current;
  const d = closing ? target.current : null;
  const opacity = Animated.multiply(
    Animated.multiply(
      o ? appear.interpolate({ inputRange: [0, 0.35, 1], outputRange: [0, 1, 1] }) : appear,
      Animated.subtract(1, mini),
    ),
    d ? out.interpolate({ inputRange: [0, 0.7, 1], outputRange: [1, 1, 0] }) : Animated.subtract(1, out),
  );
  const outScale = out.interpolate({ inputRange: [0, 1], outputRange: [1, d ? Math.min(1, d[2] / Math.max(1, g.w)) : 0.92] });
  const outX = d ? out.interpolate({ inputRange: [0, 1], outputRange: [0, d[0] + d[2] / 2 - (g.x + g.w / 2)] }) : 0;
  const outY = d ? out.interpolate({ inputRange: [0, 1], outputRange: [0, d[1] + d[3] / 2 - (frameTop + frameH / 2)] }) : 0;
  // From the icon: the frame starts at the icon's centre and size.
  const scale = o
    ? appear.interpolate({ inputRange: [0, 1], outputRange: [Math.min(1, o[2] / Math.max(1, g.w)), 1] })
    : appear.interpolate({ inputRange: [0, 1], outputRange: [0.94, 1] });
  const growX = o ? appear.interpolate({ inputRange: [0, 1], outputRange: [o[0] + o[2] / 2 - (g.x + g.w / 2), 0] }) : 0;
  const growY = o ? appear.interpolate({ inputRange: [0, 1], outputRange: [o[1] + o[3] / 2 - (frameTop + frameH / 2), 0] }) : 0;
  const miniScale = mini.interpolate({ inputRange: [0, 1], outputRange: [1, 0.25] });
  const dropY = mini.interpolate({ inputRange: [0, 1], outputRange: [0, screen.height - frameTop - frameH / 2] });

  const shadow = active
    ? `0px 18px 48px ${t.shadow}, 0px 0px 0px 1px ${t.chromeLine}`
    : `0px 4px 12px ${t.shadow}, 0px 0px 0px 1px ${t.chromeLine}`;
  const resizable = decorated && !max && win.resizable !== false && !win.snap;

  return (
    <Animated.View
      pointerEvents={hidden || win.minimized || closing ? 'none' : 'box-none'}
      style={[
        styles.frame,
        {
          left: g.x,
          top: frameTop,
          width: g.w,
          height: frameH,
          opacity: hidden ? 0 : opacity,
          transform: [
            { translateX: Animated.add(o ? Animated.add(dragX, growX) : dragX, outX) },
            { translateY: Animated.add(Animated.add(moving ? Animated.add(dropY, dragY) : dropY, growY), outY) },
            { scale: Animated.multiply(Animated.multiply(scale, miniScale), outScale) },
          ],
        },
      ]}
    >
      <View style={[styles.surface, { borderRadius: radius, boxShadow: full || max ? undefined : shadow }]}>
        {/* One title bar element for both looks: a drag that leaves the
            maximized place switches the look mid-gesture, and a new element
            would end the gesture (the release then didn't know where the
            window came from). */}
        {decorated && (
          <Animated.View
            {...move.current.panHandlers}
            {...finger.handlers}
            ref={titleAnchor.ref}
            onLayout={titleAnchor.onLayout}
            style={[
              styles.titleBar,
              { height: TB, backgroundColor: tabletChrome || active ? t.chromeActive : t.chrome, borderTopLeftRadius: radius, borderTopRightRadius: radius },
            ]}
          >
            {tabletChrome ? (
              <>
                <View style={styles.tabletName}>
                  <AppIcon win={win} app={appEntry} size={22} />
                  <Text numberOfLines={1} style={[styles.tabletTitle, { color: active ? t.text : t.textMute }]}>{appName(win, apps)}</Text>
                </View>
                <View pointerEvents="none" style={styles.grabberWrap}>
                  <View style={[styles.grabber, { backgroundColor: t.textMute }]} />
                </View>
                <View style={styles.tabletButtons}>
                  <View>
                    <Pressable ref={menuAnchor.ref} onLayout={menuAnchor.onLayout} hitSlop={4}
                      onPress={() => onMenu && onMenu(win)}
                      style={styles.titleButton} pressedStyle={{ backgroundColor: t.panelLine }}>
                      <Icon name="more" size={22} color={t.textDim} strokeWidth={2.6} />
                    </Pressable>
                    {menuOpen && <WindowMenu win={win} onClose={onMenuClose} />}
                  </View>
                </View>
              </>
            ) : (
              <>
                <View style={[styles.titleIcon, { opacity: active ? 1 : 0.6 }]}>
                  <AppIcon win={win} app={appEntry} size={26} />
                </View>
                <Text numberOfLines={1} style={[styles.title, { color: active ? t.text : t.textMute }]}>
                  {windowTitle(win, apps)}
                </Text>
                {win.type === 'normal' && <TitleButton t={t} icon="minimize" label="Minimize" anchorName={`win.${win.id}.minimize`} onPress={() => wm.minimize(win.id)} />}
                {win.type === 'normal' && win.resizable !== false && !snapped && (
                  <TitleButton t={t} icon={win.maximized ? 'restore' : 'maximize'} label="Maximize" anchorName={`win.${win.id}.maximize`}
                    onPress={() => wm.maximize(win.id, !win.maximized)} />
                )}
                <TitleButton t={t} icon="close" danger label="Close" anchorName={`win.${win.id}.close`} onPress={() => wm.close(win.id)} />
              </>
            )}
          </Animated.View>
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
