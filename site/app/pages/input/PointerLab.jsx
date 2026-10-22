// Pointer / touch demo. Left: a surface that tracks hover (onPointerMove),
// spawns a ripple where you press and reports pointer fields. Right: an orb
// dragged with PanResponder that springs into the nearest socket.
import React, { useRef, useState } from 'react';
import { Animated, Easing, PanResponder, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';

let rippleId = 0;

function Ripple({ x, y, color, onDone }) {
  const t = useRef(new Animated.Value(0)).current;
  React.useEffect(() => {
    Animated.timing(t, { toValue: 1, duration: 700, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start(onDone);
  }, []);
  return (
    <Animated.View
      pointerEvents="none"
      style={{
        position: 'absolute',
        left: x - 50,
        top: y - 50,
        width: 100,
        height: 100,
        borderRadius: 50,
        borderWidth: 2,
        borderColor: color,
        opacity: t.interpolate({ inputRange: [0, 1], outputRange: [0.9, 0] }),
        transform: [{ scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.15, 1.6] }) }],
      }}
    />
  );
}

export function HoverSurface({ height = 300 }) {
  const pos = useRef(new Animated.ValueXY({ x: 0, y: 0 })).current;
  const vis = useRef(new Animated.Value(0)).current;
  const [info, setInfo] = useState({ x: 0, y: 0, type: '—', buttons: 0, inside: false, presses: 0 });
  const [ripples, setRipples] = useState([]);
  const last = useRef(0);

  const track = (e, extra) => {
    const ne = e.nativeEvent;
    pos.setValue({ x: ne.locationX, y: ne.locationY });
    // Readout at ~20 Hz; the spotlight itself moves every event without React.
    const now = Date.now();
    if (extra || now - last.current > 50) {
      last.current = now;
      setInfo((s) => ({ ...s, x: ne.locationX, y: ne.locationY, type: ne.pointerType || 'mouse', buttons: ne.buttons || 0, ...extra }));
    }
  };
  const fade = (to) => Animated.timing(vis, { toValue: to, duration: 220, useNativeDriver: true }).start();

  return (
    <View style={{ gap: 12, flex: 1 }}>
      <View
        style={[styles.surface, { height }]}
        onPointerEnter={(e) => {
          fade(1);
          track(e, { inside: true });
        }}
        onPointerLeave={() => {
          fade(0);
          setInfo((s) => ({ ...s, inside: false, buttons: 0 }));
        }}
        onPointerMove={(e) => track(e)}
        onPointerDown={(e) => {
          const ne = e.nativeEvent;
          track(e, { presses: info.presses + 1, buttons: 1 });
          const id = ++rippleId;
          const color = [colors.cyan, colors.gold, colors.purple, colors.green][id % 4];
          setRipples((r) => [...r.slice(-5), { id, x: ne.locationX, y: ne.locationY, color }]);
        }}
        onPointerUp={(e) => track(e, { buttons: 0 })}
      >
        <View pointerEvents="none" style={styles.dots} />
        <Animated.View
          pointerEvents="none"
          style={[styles.spot, { opacity: vis, transform: [{ translateX: pos.x }, { translateY: pos.y }] }]}
        />
        <Animated.View pointerEvents="none" style={[styles.crossH, { opacity: vis, transform: [{ translateY: pos.y }] }]} />
        <Animated.View pointerEvents="none" style={[styles.crossV, { opacity: vis, transform: [{ translateX: pos.x }] }]} />
        {ripples.map((r) => (
          <Ripple key={r.id} x={r.x} y={r.y} color={r.color} onDone={() => setRipples((l) => l.filter((q) => q.id !== r.id))} />
        ))}
        <View pointerEvents="none" style={styles.surfaceHint}>
          <Text style={styles.hintText}>{info.inside ? '' : 'Hover, click or tap here'}</Text>
        </View>
      </View>
      <View style={styles.readouts}>
        <Field label="x" value={Math.round(info.x)} />
        <Field label="y" value={Math.round(info.y)} />
        <Field label="pointerType" value={info.type} />
        <Field label="buttons" value={info.buttons} />
        <Field label="presses" value={info.presses} />
      </View>
    </View>
  );
}

function Field({ label, value }) {
  return (
    <View style={styles.field}>
      <Text style={styles.fieldLabel}>{label}</Text>
      <Text style={styles.fieldValue}>{String(value)}</Text>
    </View>
  );
}

const ORB = 56;

export function DragSockets({ height = 300 }) {
  const [width, setWidth] = useState(0);
  const sockets = [
    { x: width * 0.2, y: height * 0.32 },
    { x: width * 0.78, y: height * 0.26 },
    { x: width * 0.5, y: height * 0.74 },
  ];
  const [home, setHome] = useState(2);
  const [near, setNear] = useState(-1);
  const [stats, setStats] = useState({ dx: 0, dy: 0, vx: 0, vy: 0 });
  const xy = useRef(new Animated.ValueXY(sockets[2])).current;
  const grab = useRef(new Animated.Value(0)).current;
  const base = useRef(sockets[2]);
  const live = useRef({ sockets, home });
  live.current = { sockets, home };

  const nearest = (x, y) => {
    let best = 0;
    let bd = Infinity;
    live.current.sockets.forEach((s, i) => {
      const d = (s.x - x) ** 2 + (s.y - y) ** 2;
      if (d < bd) {
        bd = d;
        best = i;
      }
    });
    return best;
  };

  const pan = useRef(
    PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: () => {
        xy.stopAnimation();
        base.current = { x: xy.x.__getValue(), y: xy.y.__getValue() };
        Animated.spring(grab, { toValue: 1, stiffness: 400, damping: 18, useNativeDriver: true }).start();
      },
      onPanResponderMove: (e, g) => {
        const x = base.current.x + g.dx;
        const y = base.current.y + g.dy;
        xy.setValue({ x, y });
        setNear(nearest(x, y));
        setStats({ dx: g.dx, dy: g.dy, vx: g.vx, vy: g.vy });
      },
      onPanResponderRelease: (e, g) => {
        const x = base.current.x + g.dx;
        const y = base.current.y + g.dy;
        // Fling-aware: project the release velocity a little ahead.
        const i = nearest(x + g.vx * 120, y + g.vy * 120);
        setHome(i);
        setNear(-1);
        Animated.spring(xy, { toValue: live.current.sockets[i], velocity: { x: g.vx, y: g.vy }, stiffness: 220, damping: 16, useNativeDriver: true }).start();
        Animated.spring(grab, { toValue: 0, stiffness: 300, damping: 14, useNativeDriver: true }).start();
      },
      onPanResponderTerminate: () => {
        const s = live.current.sockets[live.current.home];
        Animated.spring(xy, { toValue: s, stiffness: 220, damping: 16, useNativeDriver: true }).start();
        Animated.spring(grab, { toValue: 0, useNativeDriver: true }).start();
        setNear(-1);
      },
    }),
  ).current;

  // Keep the orb in its socket when the surface resizes.
  const lastW = useRef(width);
  if (lastW.current !== width) {
    lastW.current = width;
    xy.setValue(sockets[home]);
  }

  return (
    <View style={{ gap: 12, flex: 1 }}>
      <View style={[styles.surface, { height }]} onLayout={(e) => setWidth(e.nativeEvent.layout.width)}>
        <View pointerEvents="none" style={styles.dots} />
        {width > 0 && sockets.map((s, i) => (
          <View
            key={i}
            pointerEvents="none"
            style={[
              styles.socket,
              { left: s.x - 40, top: s.y - 40 },
              (near === i || (near < 0 && home === i)) && { borderColor: colors.gold, boxShadow: '0 0 30px rgba(242,195,91,0.45), inset 0 0 20px rgba(242,195,91,0.2)' },
            ]}
          />
        ))}
        <Animated.View
          {...pan.panHandlers}
          style={[
            styles.orb,
            {
              opacity: width > 0 ? 1 : 0,
              left: -ORB / 2,
              top: -ORB / 2,
              transform: [
                { translateX: xy.x },
                { translateY: xy.y },
                { scale: grab.interpolate({ inputRange: [0, 1], outputRange: [1, 1.18] }) },
              ],
            },
          ]}
        >
          <Animated.View style={[StyleSheet.absoluteFill, styles.orbGlow, { opacity: grab }]} />
          <View style={styles.orbCore} />
        </Animated.View>
        <View pointerEvents="none" style={[styles.surfaceHint, { top: 14, bottom: undefined }]}>
          <Text style={styles.hintText}>Drag the orb into a socket</Text>
        </View>
      </View>
      <View style={styles.readouts}>
        <Field label="dx" value={Math.round(stats.dx)} />
        <Field label="dy" value={Math.round(stats.dy)} />
        <Field label="vx" value={stats.vx.toFixed(2)} />
        <Field label="vy" value={stats.vy.toFixed(2)} />
        <Field label="socket" value={home + 1} />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  surface: {
    borderRadius: 16,
    overflow: 'hidden',
    backgroundColor: '#080c18',
    borderWidth: 1,
    borderColor: site.stroke,
    cursor: 'crosshair',
  },
  dots: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage: 'radial-gradient(circle at 50% 50%, rgba(79,140,255,0.10) 0%, rgba(8,12,24,0) 70%)',
  },
  spot: {
    position: 'absolute',
    left: -90,
    top: -90,
    width: 180,
    height: 180,
    borderRadius: 90,
    experimental_backgroundImage: 'radial-gradient(circle, rgba(79,209,255,0.28) 0%, rgba(79,209,255,0) 70%)',
  },
  crossH: { position: 'absolute', left: 0, right: 0, top: 0, height: 1, backgroundColor: 'rgba(79,209,255,0.35)' },
  crossV: { position: 'absolute', top: 0, bottom: 0, left: 0, width: 1, backgroundColor: 'rgba(79,209,255,0.35)' },
  surfaceHint: { position: 'absolute', left: 0, right: 0, bottom: 14, alignItems: 'center' },
  hintText: { fontFamily: font, fontWeight: '600', fontSize: 12, letterSpacing: 0.4, color: colors.textMute },
  readouts: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  field: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
    height: 28,
    paddingHorizontal: 10,
    borderRadius: 8,
    backgroundColor: 'rgba(10,15,29,0.8)',
    borderWidth: 1,
    borderColor: site.stroke,
  },
  fieldLabel: { fontFamily: mono, fontSize: 11, color: colors.textMute },
  fieldValue: { fontFamily: mono, fontWeight: '700', fontSize: 12, color: colors.text },
  socket: {
    position: 'absolute',
    width: 80,
    height: 80,
    borderRadius: 40,
    borderWidth: 2,
    borderStyle: 'dashed',
    borderColor: 'rgba(148,170,255,0.25)',
  },
  orb: { position: 'absolute', width: ORB, height: ORB, borderRadius: ORB / 2, cursor: 'grab' },
  orbGlow: { borderRadius: ORB / 2, boxShadow: '0 0 40px rgba(242,195,91,0.9)' },
  orbCore: {
    ...StyleSheet.absoluteFillObject,
    borderRadius: ORB / 2,
    experimental_backgroundImage: 'radial-gradient(circle at 35% 30%, #fff6d6 0%, #f2c35b 40%, #a8741f 100%)',
    boxShadow: '0 12px 30px rgba(0,0,0,0.6), 0 0 18px rgba(242,195,91,0.5)',
  },
});
