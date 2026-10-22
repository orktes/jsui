// Drag + fling: PanResponder moves a puck, release hands its velocity to
// Animated.decay (or to a spring when it would leave the arena, or snaps to
// the nearest corner). The fling itself runs on the native driver.
import React, { useMemo, useRef, useState } from 'react';
import { Animated, Easing, PanResponder, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';
import { DemoCard, Segmented } from './controls.jsx';

const PUCK = 64;
const DECEL = 0.997;
const HEIGHT = 300;

export function Fling() {
  const x = useRef(new Animated.Value(40)).current;
  const y = useRef(new Animated.Value(HEIGHT / 2 - PUCK / 2)).current;
  const grab = useRef(new Animated.Value(0)).current;
  const ghost = useRef(new Animated.Value(0)).current;
  const [arena, setArena] = useState({ w: 0, h: HEIGHT });
  const [mode, setMode] = useState('decay');
  const [land, setLand] = useState(null);
  const [stats, setStats] = useState(null);
  const live = useRef({});
  live.current.arena = arena;
  live.current.mode = mode;

  const pan = useMemo(() => {
    const st = { x0: 0, y0: 0, samples: [] };
    const bounds = () => ({ maxX: Math.max(0, live.current.arena.w - PUCK), maxY: live.current.arena.h - PUCK });
    // Rubber band beyond the walls while dragging.
    const rubber = (v, max) => (v < 0 ? v / 3 : v > max ? max + (v - max) / 3 : v);
    return PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: (e) => {
        x.stopAnimation((vx) => (st.x0 = vx));
        y.stopAnimation((vy) => (st.y0 = vy));
        st.samples = [{ t: Date.now(), x: 0, y: 0 }];
        Animated.spring(grab, { toValue: 1, stiffness: 500, damping: 20, useNativeDriver: true }).start();
      },
      onPanResponderMove: (e, g) => {
        const { maxX, maxY } = bounds();
        x.setValue(rubber(st.x0 + g.dx, maxX));
        y.setValue(rubber(st.y0 + g.dy, maxY));
        st.samples.push({ t: Date.now(), x: g.dx, y: g.dy });
        if (st.samples.length > 8) st.samples.shift();
      },
      onPanResponderRelease: (e, g) => {
        Animated.spring(grab, { toValue: 0, stiffness: 300, damping: 14, useNativeDriver: true }).start();
        const { maxX, maxY } = bounds();
        // Velocity from the moves in the last ~120 ms before release; a
        // finger that rested before letting go has none.
        const s = st.samples;
        const rel = Date.now();
        const last = s[s.length - 1];
        let first = s.length > 1 ? s[s.length - 2] : last;
        for (const p of s) if (last.t - p.t <= 120) { first = p; break; }
        if (first === last && s.length > 1) first = s[s.length - 2];
        const dt = Math.max(1, last.t - first.t);
        const rested = rel - last.t > 90;
        let vx = rested ? 0 : (last.x - first.x) / dt;
        let vy = rested ? 0 : (last.y - first.y) / dt;
        const cap = 6;
        vx = Math.max(-cap, Math.min(cap, vx));
        vy = Math.max(-cap, Math.min(cap, vy));
        const px = st.x0 + g.dx;
        const py = st.y0 + g.dy;
        const speed = Math.hypot(vx, vy);
        setStats({ speed: speed.toFixed(2) });
        const fling = (val, pos, v, max) => {
          const projected = pos + v / (1 - DECEL);
          if (live.current.mode === 'corners') {
            const target = projected < max / 2 ? 0 : max;
            Animated.spring(val, { toValue: target, velocity: v, stiffness: 180, damping: 18, useNativeDriver: true }).start();
            return target;
          }
          if (pos >= 0 && pos <= max && projected >= 0 && projected <= max && Math.abs(v) > 0.02) {
            Animated.decay(val, { velocity: v, deceleration: DECEL, useNativeDriver: true }).start();
            return projected;
          }
          // Would hit (or is past) a wall: spring onto it, keeping the velocity.
          const target = Math.max(0, Math.min(max, projected));
          Animated.spring(val, { toValue: target, velocity: v, stiffness: 260, damping: 16, useNativeDriver: true }).start();
          return target;
        };
        const lx = fling(x, px, vx, maxX);
        const ly = fling(y, py, vy, maxY);
        setLand({ x: lx, y: ly });
        ghost.setValue(1);
        Animated.timing(ghost, { toValue: 0, duration: 1400, delay: 200, easing: Easing.in(Easing.quad), useNativeDriver: true }).start();
      },
    });
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const scale = grab.interpolate({ inputRange: [0, 1], outputRange: [1, 1.12] });
  return (
    <DemoCard
      index={6}
      title="Drag and fling"
      blurb="PanResponder tracks the drag (with a rubber band past the walls). On release the velocity goes to Animated.decay, the same exponential friction as a ScrollView fling, or to a spring that lands on a wall or a corner. The ghost ring marks where the math says it will stop."
      tags={[['PanResponder', colors.cyan], ['Animated.decay', colors.gold], ['velocity handoff', colors.purple]]}
      codeTitle="fling.jsx"
      code={`
const x = useRef(new Animated.Value(0)).current;

const pan = PanResponder.create({
  onStartShouldSetPanResponder: () => true,
  onPanResponderGrant: () =>
    x.stopAnimation((v) => (start = v)),
  onPanResponderMove: (e, g) => x.setValue(start + g.dx),
  onPanResponderRelease: (e, { vx }) => {
    // Lands at start + dx + vx / (1 - deceleration).
    Animated.decay(x, {
      velocity: vx,          // px / ms
      deceleration: 0.997,
      useNativeDriver: true, // the glide runs in C++
    }).start();
  },
});`}
    >
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, flexWrap: 'wrap' }}>
        <Segmented
          options={[
            { id: 'decay', label: 'Decay + walls' },
            { id: 'corners', label: 'Snap to corners' },
          ]}
          value={mode}
          onChange={setMode}
          color={colors.gold}
          small
        />
        <View style={{ flex: 1 }} />
        <Text style={styles.stat}>{stats ? `release ${stats.speed} px/ms` : 'drag the puck and let go'}</Text>
      </View>
      <View style={styles.arena} onLayout={(e) => setArena({ w: e.nativeEvent.layout.width - 2, h: e.nativeEvent.layout.height - 2 })}>
        <View pointerEvents="none" style={styles.gridBg} />
        {mode === 'corners'
          ? [
              [0, 0],
              [1, 0],
              [0, 1],
              [1, 1],
            ].map(([cx, cy]) => (
              <View key={`${cx}${cy}`} pointerEvents="none" style={[styles.corner, { left: cx ? arena.w - PUCK - 4 : -4, top: cy ? arena.h - PUCK - 4 : -4 }]} />
            ))
          : null}
        {land ? (
          <Animated.View
            pointerEvents="none"
            style={[styles.ghost, { left: land.x, top: land.y, opacity: ghost, transform: [{ scale: ghost.interpolate({ inputRange: [0, 1], outputRange: [1.4, 1] }) }] }]}
          />
        ) : null}
        <Animated.View
          {...pan.panHandlers}
          style={[styles.puck, { cursor: 'grab', transform: [{ translateX: x }, { translateY: y }, { scale }] }]}
        >
          <View pointerEvents="none" style={styles.puckInner} />
        </Animated.View>
      </View>
    </DemoCard>
  );
}

const styles = StyleSheet.create({
  stat: { fontFamily: mono, fontSize: 12, color: colors.textMute },
  arena: { height: HEIGHT + 2, borderRadius: 14, borderWidth: 1, borderColor: site.strokeStrong, backgroundColor: 'rgba(0,0,0,0.3)', overflow: 'hidden' },
  gridBg: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage:
      'radial-gradient(circle at 50% 50%, rgba(79,140,255,0.12) 0%, rgba(5,7,15,0) 70%), linear-gradient(90deg, rgba(148,170,255,0.06) 1px, rgba(0,0,0,0) 1px)',
  },
  corner: { position: 'absolute', width: PUCK + 8, height: PUCK + 8, borderRadius: (PUCK + 8) / 2, borderWidth: 1, borderStyle: 'dashed', borderColor: 'rgba(242,195,91,0.35)' },
  ghost: { position: 'absolute', width: PUCK, height: PUCK, borderRadius: PUCK / 2, borderWidth: 2, borderColor: colors.cyan, boxShadow: '0 0 16px rgba(79,209,255,0.6)' },
  puck: {
    position: 'absolute',
    left: 0,
    top: 0,
    width: PUCK,
    height: PUCK,
    borderRadius: PUCK / 2,
    alignItems: 'center',
    justifyContent: 'center',
    experimental_backgroundImage: 'radial-gradient(circle at 35% 30%, #fff2c2 0%, #f2c35b 45%, #a8741f 100%)',
    boxShadow: '0 12px 30px rgba(0,0,0,0.6), 0 0 24px rgba(242,195,91,0.45)',
  },
  puckInner: { width: 22, height: 22, borderRadius: 11, borderWidth: 2, borderColor: 'rgba(42,26,3,0.45)' },
});
