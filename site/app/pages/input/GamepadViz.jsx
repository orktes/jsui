// Live gamepad visualizer: a controller drawn with Views that mirrors
// useGamepad() (W3C standard mapping). Buttons light up with springs,
// sticks follow the axes, triggers fill. Without a pad it waits for one.
import React, { memo, useEffect, useRef, useState } from 'react';
import { Animated, Easing, StyleSheet, Text, View, useGamepad } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';

export const BUTTON_NAMES = ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'View', 'Menu', 'LS', 'RS', 'D-pad ↑', 'D-pad ↓', 'D-pad ←', 'D-pad →', 'Home'];

const FACE = { A: colors.green, B: colors.red, X: colors.blue, Y: colors.gold };
const BODY = '#18213a';

const springTo = (v, to) => Animated.spring(v, { toValue: to, stiffness: 420, damping: 22, mass: 0.7, useNativeDriver: true });

/** A shape that lights up (opacity + glow) and squashes while `on`. */
const Lit = memo(function Lit({ on, color, style, radius, children, squash = 0.9 }) {
  const v = useRef(new Animated.Value(on ? 1 : 0)).current;
  useEffect(() => {
    springTo(v, on ? 1 : 0).start();
  }, [on]);
  const scale = v.interpolate({ inputRange: [0, 1], outputRange: [1, squash] });
  return (
    <Animated.View style={[style, { transform: [{ scale }] }]}>
      <View style={[StyleSheet.absoluteFill, { borderRadius: radius, backgroundColor: '#0f1628', borderWidth: 1, borderColor: color + '55' }]} />
      <Animated.View
        style={[
          StyleSheet.absoluteFill,
          { borderRadius: radius, backgroundColor: color, opacity: v, boxShadow: `0 0 22px ${color}` },
        ]}
      />
      <View style={[StyleSheet.absoluteFill, { alignItems: 'center', justifyContent: 'center' }]}>{children}</View>
    </Animated.View>
  );
});

function Glyph({ children, on, color = colors.text, size = 15 }) {
  return <Text style={{ fontFamily: font, fontWeight: '700', fontSize: size, color: on ? '#0b1020' : color }}>{children}</Text>;
}

const Stick = memo(function Stick({ x, y, pressed, left, top, label }) {
  const R = 40;
  return (
    <View style={{ position: 'absolute', left: left - R, top: top - R, width: R * 2, height: R * 2 }}>
      <View style={[styles.stickWell]} />
      {/* travel indicator */}
      <View pointerEvents="none" style={[styles.stickTrail, { transform: [{ translateX: x * 22 }, { translateY: y * 22 }] }]} />
      <Lit
        on={pressed}
        color={colors.cyan}
        radius={26}
        squash={0.88}
        style={{ position: 'absolute', left: R - 26, top: R - 26, width: 52, height: 52, transform: [{ translateX: x * 22 }, { translateY: y * 22 }] }}
      >
        <View style={styles.stickCap} />
        <Text style={[styles.stickLabel, pressed && { color: '#0b1020' }]}>{label}</Text>
      </Lit>
    </View>
  );
});

function Trigger({ value, left, label }) {
  const v = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    springTo(v, value).start();
  }, [value]);
  return (
    <View style={[styles.trigger, { left }]}>
      <Animated.View style={[styles.triggerFill, { transform: [{ scaleY: v }], opacity: v.interpolate({ inputRange: [0, 1], outputRange: [0.4, 1] }) }]} />
      <Text style={styles.triggerLabel}>{label}</Text>
    </View>
  );
}

function Pulse() {
  const t = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const loop = Animated.loop(Animated.timing(t, { toValue: 1, duration: 1600, easing: Easing.out(Easing.cubic), useNativeDriver: true }));
    loop.start();
    return () => loop.stop();
  }, []);
  return (
    <View style={{ width: 64, height: 64, alignItems: 'center', justifyContent: 'center' }}>
      {[0, 1].map((i) => (
        <Animated.View
          key={i}
          style={{
            position: 'absolute',
            width: 64,
            height: 64,
            borderRadius: 32,
            borderWidth: 2,
            borderColor: colors.gold,
            opacity: t.interpolate({ inputRange: [0, 1], outputRange: i ? [0.5, 0] : [0.9, 0.1] }),
            transform: [{ scale: t.interpolate({ inputRange: [0, 1], outputRange: i ? [0.6, 1.5] : [0.4, 1] }) }],
          }}
        />
      ))}
      <View style={[styles.face, { width: 30, height: 30, borderRadius: 15, backgroundColor: colors.green, position: 'relative', left: 0, top: 0 }]}>
        <Glyph on>A</Glyph>
      </View>
    </View>
  );
}

/** The controller, drawn at 600 x 360. */
export function Controller({ pad }) {
  const b = (i) => (pad.buttons[i] || 0) > 0.05;
  const a = (i) => {
    const v = pad.axes[i] || 0;
    return Math.abs(v) < 0.04 ? 0 : v;
  };
  return (
    <View style={styles.controller}>
      <Trigger value={b(6) ? Math.max(0.35, pad.buttons[6]) : 0} left={118} label="LT" />
      <Trigger value={b(7) ? Math.max(0.35, pad.buttons[7]) : 0} left={426} label="RT" />
      <View style={[styles.grip, { left: 70, transform: [{ rotate: '22deg' }] }]} />
      <View style={[styles.grip, { left: 380, transform: [{ rotate: '-22deg' }] }]} />
      <View style={styles.body} />
      <View pointerEvents="none" style={styles.bodySheen} />
      <Lit on={b(4)} color={colors.cyan} radius={14} style={[styles.bumper, { left: 96 }]}>
        <Glyph on={b(4)} size={12}>LB</Glyph>
      </Lit>
      <Lit on={b(5)} color={colors.cyan} radius={14} style={[styles.bumper, { left: 404 }]}>
        <Glyph on={b(5)} size={12}>RB</Glyph>
      </Lit>


      <Stick x={a(0)} y={a(1)} pressed={b(10)} left={176} top={138} label="L" />
      <Stick x={a(2)} y={a(3)} pressed={b(11)} left={370} top={218} label="R" />

      {/* D-pad */}
      <View style={styles.dpad}>
        <View style={styles.dpadCross} />
        <Lit on={b(12)} color={colors.text} radius={6} style={[styles.dpadArm, { left: 30, top: 2 }]}>
          <Glyph on={b(12)} size={11} color={colors.textMute}>▲</Glyph>
        </Lit>
        <Lit on={b(13)} color={colors.text} radius={6} style={[styles.dpadArm, { left: 30, top: 58 }]}>
          <Glyph on={b(13)} size={11} color={colors.textMute}>▼</Glyph>
        </Lit>
        <Lit on={b(14)} color={colors.text} radius={6} style={[styles.dpadArm, { left: 2, top: 30 }]}>
          <Glyph on={b(14)} size={11} color={colors.textMute}>◀</Glyph>
        </Lit>
        <Lit on={b(15)} color={colors.text} radius={6} style={[styles.dpadArm, { left: 58, top: 30 }]}>
          <Glyph on={b(15)} size={11} color={colors.textMute}>▶</Glyph>
        </Lit>
      </View>

      {/* Face buttons */}
      {[
        [3, 'Y', 424, 92],
        [2, 'X', 384, 132],
        [1, 'B', 464, 132],
        [0, 'A', 424, 172],
      ].map(([i, l, x, y]) => (
        <Lit key={l} on={b(i)} color={FACE[l]} radius={19} style={[styles.face, { left: x - 19, top: y - 19 }]}>
          <Glyph on={b(i)} color={FACE[l]}>
            {l}
          </Glyph>
        </Lit>
      ))}

      {/* Center */}
      <Lit on={b(8)} color={colors.text} radius={9} style={[styles.small, { left: 252 }]} />
      <Lit on={b(9)} color={colors.text} radius={9} style={[styles.small, { left: 330 }]} />
      <Lit on={b(16)} color={colors.gold} radius={18} style={styles.home}>
        <View style={{ width: 14, height: 14, borderRadius: 4, borderWidth: 2, borderColor: b(16) ? '#0b1020' : colors.gold, transform: [{ rotate: '45deg' }] }} />
      </Lit>
    </View>
  );
}

const EMPTY = { connected: false, id: '', buttons: [], axes: [] };

/** First connected pad among indices 0-3. */
function useAnyPad(onButton) {
  const p0 = useGamepad({ index: 0, onButton });
  const p1 = useGamepad({ index: 1, onButton });
  const p2 = useGamepad({ index: 2, onButton });
  const p3 = useGamepad({ index: 3, onButton });
  return [p0, p1, p2, p3].find((p) => p.connected) || p0 || EMPTY;
}

function AxisBar({ label, v }) {
  const pct = Math.max(-1, Math.min(1, v || 0));
  return (
    <View style={styles.axisRow}>
      <Text style={styles.axisLabel}>{label}</Text>
      <View style={styles.axisTrack}>
        <View style={styles.axisMid} />
        <View
          style={[
            styles.axisFill,
            pct >= 0 ? { left: '50%', width: `${pct * 50}%` } : { right: '50%', width: `${-pct * 50}%` },
          ]}
        />
      </View>
      <Text style={styles.axisValue}>{(pct >= 0 ? '+' : '') + pct.toFixed(2)}</Text>
    </View>
  );
}

function WaitText() {
  return (
    <View style={{ gap: 8, alignItems: 'center' }}>
      <Text style={styles.waitTitle}>Connect a controller and press any button</Text>
      <Text style={styles.waitBody}>Browsers reveal a gamepad to the page on its first button press.</Text>
    </View>
  );
}

let evId = 0;

export function GamepadPanel({ phone, width }) {
  const [events, setEvents] = useState([]);
  const pad = useAnyPad((e) => {
    const ne = e.nativeEvent || e;
    const pressed = ne.pressed != null ? ne.pressed : (ne.value || 0) > 0.5;
    if (ne.repeat) return;
    const item = { id: ++evId, name: BUTTON_NAMES[e.buttonIndex] || `#${e.buttonIndex}`, index: e.buttonIndex, pressed };
    setEvents((list) => [item, ...list].slice(0, 6));
  });
  const connected = pad.connected;
  const vizW = Math.min(width, 600);
  const s = vizW / 600;

  return (
    <View style={[styles.panel, phone && { flexDirection: 'column' }]}>
      <View style={{ width: vizW, height: 360 * s, alignSelf: 'center' }}>
        <View style={{ position: 'absolute', left: (vizW - 600) / 2, top: (360 * s - 360) / 2, width: 600, height: 360, transform: [{ scale: s }] }}>
          <View style={{ flex: 1, opacity: connected ? 1 : 0.16 }}>
            <Controller pad={pad} />
          </View>
          {!connected ? (
            <View style={styles.waiting}>
              <Pulse />
              {s >= 0.8 ? <WaitText /> : null}
            </View>
          ) : null}
        </View>
      </View>

      {!connected && s < 0.8 ? <WaitText /> : null}
      <View style={[styles.side, phone && { width: '100%' }]}>
        <View style={styles.sideHead}>
          <View style={[styles.dot, { backgroundColor: connected ? colors.green : colors.textMute }]} />
          <Text style={styles.sideTitle} numberOfLines={1}>
            {connected ? pad.id || 'Gamepad connected' : 'No gamepad yet'}
          </Text>
        </View>
        <View style={{ gap: 8 }}>
          <AxisBar label="LX" v={pad.axes[0]} />
          <AxisBar label="LY" v={pad.axes[1]} />
          <AxisBar label="RX" v={pad.axes[2]} />
          <AxisBar label="RY" v={pad.axes[3]} />
        </View>
        <Text style={styles.sideCaption}>BUTTON EVENTS</Text>
        <View style={{ gap: 6, minHeight: 6 * 26 }}>
          {events.length ? (
            events.map((ev, i) => (
              <View key={ev.id} style={[styles.evRow, { opacity: 1 - i * 0.13 }]}>
                <Text style={[styles.evDir, { color: ev.pressed ? colors.green : colors.textMute }]}>{ev.pressed ? 'down' : 'up'}</Text>
                <Text style={styles.evName}>{ev.name}</Text>
                <Text style={styles.evIdx}>button {ev.index}</Text>
              </View>
            ))
          ) : (
            <Text style={styles.evEmpty}>Events from useGamepad({'{ onButton }'}) appear here.</Text>
          )}
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  panel: { flexDirection: 'row', gap: 28, alignItems: 'center' },
  controller: { width: 600, height: 360 },
  body: {
    position: 'absolute',
    left: 74,
    top: 66,
    width: 452,
    height: 200,
    borderRadius: 100,
    backgroundColor: BODY,
    boxShadow: '0 30px 60px rgba(0,0,0,0.55)',
  },
  bodySheen: {
    position: 'absolute',
    left: 100,
    top: 68,
    width: 400,
    height: 90,
    borderRadius: 80,
    experimental_backgroundImage: 'linear-gradient(180deg, rgba(148,170,255,0.12) 0%, rgba(148,170,255,0) 100%)',
  },
  grip: {
    position: 'absolute',
    top: 150,
    width: 150,
    height: 190,
    borderRadius: 75,
    backgroundColor: BODY,
    boxShadow: '0 30px 60px rgba(0,0,0,0.5)',
  },
  trigger: {
    position: 'absolute',
    top: 4,
    width: 56,
    height: 40,
    borderRadius: 12,
    borderBottomLeftRadius: 4,
    borderBottomRightRadius: 4,
    overflow: 'hidden',
    backgroundColor: '#0f1628',
    borderWidth: 1,
    borderColor: 'rgba(79, 209, 255, 0.35)',
    alignItems: 'center',
    justifyContent: 'center',
  },
  triggerFill: {
    ...StyleSheet.absoluteFillObject,
    transformOrigin: 'bottom',
    experimental_backgroundImage: 'linear-gradient(0deg, #1477b8 0%, #4fd1ff 100%)',
  },
  triggerLabel: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.text },
  bumper: { position: 'absolute', top: 50, width: 100, height: 24 },
  stickWell: {
    ...StyleSheet.absoluteFillObject,
    borderRadius: 40,
    backgroundColor: '#0a0f1d',
    borderWidth: 1,
    borderColor: 'rgba(148,170,255,0.18)',
    boxShadow: 'inset 0 4px 12px rgba(0,0,0,0.6)',
  },
  stickTrail: { position: 'absolute', left: 36, top: 36, width: 8, height: 8, borderRadius: 4, backgroundColor: 'rgba(79,209,255,0.5)' },
  stickCap: {
    position: 'absolute',
    left: 6,
    top: 6,
    right: 6,
    bottom: 6,
    borderRadius: 20,
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.08)',
  },
  stickLabel: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.textMute },
  dpad: { position: 'absolute', left: 190, top: 180, width: 88, height: 88 },
  dpadCross: { position: 'absolute', left: 28, top: 0, width: 32, height: 88, borderRadius: 8, backgroundColor: '#0a0f1d' },
  dpadArm: { position: 'absolute', width: 28, height: 28 },
  face: { position: 'absolute', width: 38, height: 38, alignItems: 'center', justifyContent: 'center' },
  small: { position: 'absolute', top: 118, width: 18, height: 18 },
  home: { position: 'absolute', left: 282, top: 82, width: 36, height: 36 },
  waiting: { ...StyleSheet.absoluteFillObject, alignItems: 'center', justifyContent: 'center', gap: 10, paddingHorizontal: 40 },
  waitTitle: { fontFamily: font, fontWeight: '700', fontSize: 20, color: colors.text, textAlign: 'center', textShadowColor: 'rgba(0,0,0,0.8)', textShadowRadius: 12 },
  waitBody: { fontFamily: font, fontSize: 14, color: colors.textDim, textAlign: 'center' },
  side: { flex: 1, maxWidth: 420, minWidth: 260, gap: 14 },
  sideHead: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  dot: { width: 8, height: 8, borderRadius: 4 },
  sideTitle: { flex: 1, fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
  sideCaption: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2, color: colors.textMute, marginTop: 6 },
  axisRow: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  axisLabel: { width: 22, fontFamily: mono, fontSize: 12, color: colors.textDim },
  axisTrack: { flex: 1, height: 8, borderRadius: 4, backgroundColor: '#0a0f1d', overflow: 'hidden' },
  axisMid: { position: 'absolute', left: '50%', top: 0, bottom: 0, width: 1, backgroundColor: site.strokeStrong },
  axisFill: { position: 'absolute', top: 0, bottom: 0, backgroundColor: colors.cyan },
  axisValue: { width: 48, textAlign: 'right', fontFamily: mono, fontSize: 12, color: colors.text },
  evRow: { flexDirection: 'row', alignItems: 'center', gap: 10, height: 20 },
  evDir: { width: 40, fontFamily: mono, fontSize: 12 },
  evName: { flex: 1, fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },
  evIdx: { fontFamily: mono, fontSize: 11, color: colors.textMute },
  evEmpty: { fontFamily: font, fontSize: 13, lineHeight: 19, color: colors.textMute },
});
