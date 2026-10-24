// End-to-end fixture for tests/native/react_derive_e2e_test.cpp:
// Animated.derive expressions (compiled by jsui build, evaluated natively)
// and Animated.clock. 400 x 300.
// - A nameplate follows host frame data (buffer 'unit': x, y, hp; 'cam': x)
//   through derived values that clamp, branch and capture constants and an
//   Animated value: the host checks them against the same math every frame,
//   with JS ticked and not ticked (jsui_tick_native).
// - JS-driven twins of the same expressions over plain values: the JS
//   evaluator and the native one agree.
// - A clock-driven box; a function outside the subset fails to go native.
import React, { useEffect, useMemo } from 'react';
import { AppRegistry, View, Animated } from 'react-native';
import { Host, useHostMessage, useHostBufferValue, clamp, Expr } from '@orktes/jsui-react';

const post = (name, value) => Host.postMessage(name, value);
const reg = {};  // name -> host component (its _nativeTag is the node id)
const SCALE = 1.5;
const MARGIN = 8;

// The expressions under test, each (a, b) => number, shared by the native
// and JS-driven twins.
const exprs = {
  arith: Animated.expr((a, b) => (a * 3 - b / 4) % 7 + 2 ** (b / 50)),
  logic: Animated.expr((a, b) => (a > b && a < 100 ? 1 : 0) + (!(a === b) || b >= 50 ? 10 : 20)),
  branches: Animated.expr((a, b) => {
    const d = a - b;
    if (d > 20) return 20 + Math.sqrt(d);
    else if (d < -20) {
      return -20 - Math.cbrt(-d);
    }
    return d;
  }),
  math: Animated.expr((a, b) => Math.sin(a / 10) * 40 + Math.max(a, b, 30) - Math.min(a, 5) + Math.hypot(a, b) / 10 + Math.atan2(a, b + 1)),
  helpers: Animated.expr((a, b) => clamp(a, 10, 60) + Expr.lerp(0, 100, Expr.smoothstep(0, 100, b)) + Expr.wrap(a - b, 30) + Expr.fract(a / 7)),
  rounding: Animated.expr((a, b) => Math.round(a / 3) + Math.floor(b / 7) + Math.ceil(a / 9) + Math.trunc(-b / 11) + Math.abs(a - 50) + Math.sign(a - b)),
};

function Twins({ values }) {
  return Object.keys(exprs).map((name) =>
    values.map((pair, k) => {
      const v = Animated.derive(pair, exprs[name]);
      return <Animated.View key={name + k} ref={(r) => (reg[name + k] = r)} style={{ position: 'absolute', width: 4, height: 4, transform: [{ translateX: v }] }} />;
    }),
  );
}

function Nameplate({ offset }) {
  const x = useHostBufferValue('unit', 0);
  const y = useHostBufferValue('unit', 1);
  const hp = useHostBufferValue('unit', 2);
  const cam = useHostBufferValue('cam', 0);
  const style = useMemo(() => {
    // Captures SCALE, MARGIN (constants) and offset (an Animated value).
    const left = Animated.derive([x, cam], (ux, cx) => clamp((ux - cx) * SCALE + offset, MARGIN, 400 - MARGIN));
    const top = Animated.derive([y], (uy) => {
      if (uy < 0) return MARGIN;
      return Math.min(uy, 300 - MARGIN);
    });
    const width = Animated.derive([hp], (h) => Expr.smoothstep(0, 100, h));
    return { left, top, width };
  }, [x, y, hp, cam, offset]);
  return (
    <Animated.View
      ref={(r) => (reg.plate = r)}
      style={{ position: 'absolute', width: 40, height: 6, transform: [{ translateX: style.left }, { translateY: style.top }, { scaleX: style.width }] }}
    />
  );
}

function App() {
  const offset = useMemo(() => new Animated.Value(5, { useNativeDriver: true }), []);
  const native = useMemo(() => [0, 1].map(() => [new Animated.Value(0, { useNativeDriver: true }), new Animated.Value(0, { useNativeDriver: true })]), []);
  const js = useMemo(() => [0, 1].map(() => [new Animated.Value(0), new Animated.Value(0)]), []);
  const clockX = useMemo(() => Animated.derive([Animated.clock()], (s) => (s * 1000) % 1000), []);
  useHostMessage('set', (v) => {
    native.forEach((p, k) => p.forEach((n, i) => n.setValue(v[k * 2 + i])));
    js.forEach((p, k) => p.forEach((n, i) => n.setValue(v[k * 2 + i])));
  });
  useEffect(() => {
    const ids = {};
    for (const k in reg) ids[k] = reg[k] && reg[k]._nativeTag;
    post('ids', ids);
  }, []);
  useHostMessage('offset', (v) => offset.setValue(v));
  useHostMessage('notNative', () => {
    const v = new Animated.Value(1, { useNativeDriver: true });
    try {
      Animated.derive([v], (a) => [a].length);
      post('notNative', 'no error');
    } catch (e) {
      post('notNative', String(e.message));
    }
    // The JS driver still runs a function outside the subset.
    post('jsOnly', Animated.derive([new Animated.Value(3)], (a) => [a, a].length * a).__getValue());
  });
  return (
    <View style={{ flex: 1 }}>
      <Nameplate offset={offset} />
      <View>
        <Twins values={native} />
      </View>
      <Animated.View ref={(r) => (reg.clock = r)} style={{ position: 'absolute', width: 4, height: 4, transform: [{ translateX: clockX }] }} />
      <JsTwins js={js} />
    </View>
  );
}

function JsTwins({ js }) {
  return Object.keys(exprs).map((name) =>
    js.map((pair, k) => {
      const v = Animated.derive(pair, exprs[name]);
      return <Animated.View key={name + k} ref={(r) => (reg['js_' + name + (k)] = r)} style={{ position: 'absolute', width: 4, height: 4, transform: [{ translateX: v }] }} />;
    }),
  );
}

AppRegistry.registerComponent('Main', () => App);
