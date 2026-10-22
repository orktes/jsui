// End-to-end fixture for tests/native/react_native_animated_e2e_test.cpp:
// Animated with useNativeDriver: true next to the same animations on the JS
// driver (values compared frame by frame), animations progressing while JS
// is not ticked (jsui_tick_native), end callbacks, stop / setValue / listener
// sync, native loops, Animated.event on a ScrollView, and RN's errors for
// layout props. 400 x 300.
import React, { useEffect, useRef } from 'react';
import { AppRegistry, View, Animated, Easing, StyleSheet } from 'react-native';
import { Host, useHostMessage } from '@jsui/react';

const post = (name, value) => Host.postMessage(name, value);

// Each pair animates the same style with the same config: index 0 on the JS
// driver, index 1 on the native driver.
const PAIRS = {
  ease: {
    anim: (v, nd) => Animated.timing(v, { toValue: 1, duration: 611, useNativeDriver: nd }),
    style: (v) => ({ opacity: v }),
  },
  bounce: {
    anim: (v, nd) => Animated.timing(v, { toValue: 200, duration: 505, easing: Easing.bounce, useNativeDriver: nd }),
    style: (v) => ({ transform: [{ translateX: v }] }),
  },
  elastic: {
    anim: (v, nd) => Animated.timing(v, { toValue: 1, duration: 455, delay: 32, easing: Easing.elastic(2), useNativeDriver: nd }),
    style: (v) => ({ transform: [{ scale: v }, { translateX: 3 }] }),
  },
  bezier: {
    anim: (v, nd) => Animated.timing(v, { toValue: 1, duration: 300, easing: Easing.bezier(0.25, 0.1, 0.25, 1), useNativeDriver: nd }),
    style: (v) => ({ opacity: v }),
  },
  springOrigami: {
    anim: (v, nd) => Animated.spring(v, { toValue: 1, useNativeDriver: nd }),
    style: (v) => ({ transform: [{ translateY: v.interpolate({ inputRange: [0, 1], outputRange: [0, 100] }) }] }),
  },
  springBouncy: {
    anim: (v, nd) => Animated.spring(v, { toValue: 1, bounciness: 14, speed: 9, useNativeDriver: nd }),
    style: (v) => ({ opacity: v }),
  },
  springPhysics: {
    anim: (v, nd) =>
      Animated.spring(v, { toValue: 300, stiffness: 180, damping: 12, mass: 1.5, velocity: 4, useNativeDriver: nd }),
    style: (v) => ({ transform: [{ translateX: v }] }),
  },
  springClamped: {
    anim: (v, nd) => Animated.spring(v, { toValue: 1, overshootClamping: true, tension: 80, friction: 4, useNativeDriver: nd }),
    style: (v) => ({ opacity: v }),
  },
  decay: {
    anim: (v, nd) => Animated.decay(v, { velocity: 0.9, deceleration: 0.996, useNativeDriver: nd }),
    style: (v) => ({ transform: [{ translateX: v }] }),
  },
  color: {
    anim: (v, nd) => Animated.timing(v, { toValue: 1, duration: 400, easing: Easing.linear, useNativeDriver: nd }),
    style: (v) => ({
      backgroundColor: v.interpolate({ inputRange: [0, 0.5, 1], outputRange: ['#ff0000', 'rgba(0, 255, 0, 0.5)', 'blue'] }),
      transform: [
        { rotate: v.interpolate({ inputRange: [0, 1], outputRange: ['0deg', '270deg'] }) },
        { translateX: Animated.multiply(Animated.add(v, 1), 20) },
        { translateY: Animated.modulo(Animated.multiply(v, 70), 25) },
        { scale: v.interpolate({ inputRange: [0, 0.5], outputRange: [1, 2], extrapolate: 'clamp' }) },
      ],
      borderColor: v.interpolate({ inputRange: [0, 1], outputRange: ['#000000', '#ffffff'], easing: Easing.quad }),
      opacity: Animated.diffClamp(Animated.multiply(v, 3), 0, 1),
    }),
  },
};
const NAMES = Object.keys(PAIRS);

function Pair({ name, values, refs }) {
  const p = PAIRS[name];
  return (
    <View style={styles.row}>
      {[0, 1].map((i) => (
        <Animated.View key={i} ref={(r) => (refs.current[name + i] = r)} style={[styles.box, p.style(values[i])]} />
      ))}
    </View>
  );
}

function App() {
  const values = useRef(null);
  if (!values.current) {
    values.current = {};
    for (const n of NAMES) values.current[n] = [new Animated.Value(0), new Animated.Value(0)];
  }
  const refs = useRef({});
  // Scenario values.
  const blockedJs = useRef(new Animated.Value(0)).current;
  const blockedNative = useRef(new Animated.Value(0)).current;
  const stopV = useRef(new Animated.Value(0)).current;
  const loopV = useRef(new Animated.Value(0)).current;
  const scrollY = useRef(new Animated.Value(0)).current;
  const widthV = useRef(new Animated.Value(10)).current;
  const extra = useRef({}).current;

  useHostMessage('parity', () => {
    for (const n of NAMES) {
      [0, 1].forEach((i) => {
        PAIRS[n].anim(values.current[n][i], i === 1).start((r) => post('end', { name: n, driver: i ? 'native' : 'js', finished: r.finished }));
      });
    }
  });
  useHostMessage('blocked', () => {
    Animated.timing(blockedJs, { toValue: 1, duration: 320, easing: Easing.linear, useNativeDriver: false }).start((r) =>
      post('blockedEnd', { driver: 'js', finished: r.finished }),
    );
    Animated.timing(blockedNative, { toValue: 1, duration: 320, easing: Easing.linear, useNativeDriver: true }).start((r) =>
      post('blockedEnd', { driver: 'native', finished: r.finished, value: r.value }),
    );
  });
  useHostMessage('stopStart', () => {
    extra.updates = 0;
    extra.listener = stopV.addListener(({ value }) => {
      extra.updates++;
      extra.last = value;
    });
    extra.anim = Animated.timing(stopV, { toValue: 100, duration: 1000, easing: Easing.linear, useNativeDriver: true });
    extra.anim.start((r) => post('stopEnd', r));
  });
  useHostMessage('stopRead', () => post('stopRead', { value: stopV.__getValue(), listenerUpdates: extra.updates, listenerLast: extra.last }));
  useHostMessage('stop', () => stopV.stopAnimation((v) => post('stopValue', v)));
  useHostMessage('setValue', () => {
    stopV.setValue(42);
    post('afterSet', stopV.__getValue());
  });
  useHostMessage('restart', () => {
    // A JS-driven animation on a value moved to native throws (React Native).
    try {
      Animated.timing(stopV, { toValue: 0, duration: 100, useNativeDriver: false }).start();
      post('restart', 'no error');
    } catch (e) {
      post('restart', String(e.message));
    }
  });
  useHostMessage('loop', () => {
    Animated.loop(Animated.timing(loopV, { toValue: 1, duration: 96, easing: Easing.linear, useNativeDriver: true }), {
      iterations: 3,
    }).start((r) => post('loopEnd', { finished: r.finished }));
  });
  useHostMessage('layoutProp', () => {
    try {
      Animated.timing(widthV, { toValue: 100, duration: 100, useNativeDriver: true }).start();
      post('layoutProp', 'no error');
    } catch (e) {
      post('layoutProp', String(e.message));
    }
  });

  useEffect(() => {
    const ids = {};
    for (const k in refs.current) ids[k] = refs.current[k] && refs.current[k]._nativeTag;
    post('ids', ids);
  }, []);

  const onScroll = Animated.event([{ nativeEvent: { contentOffset: { y: scrollY } } }], { useNativeDriver: true });
  return (
    <View style={styles.root}>
      <View style={styles.pairs}>
        {NAMES.map((n) => (
          <Pair key={n} name={n} values={values.current[n]} refs={refs} />
        ))}
      </View>
      <Animated.View ref={(r) => (refs.current.blockedJs = r)} style={[styles.big, { left: 200, opacity: blockedJs }]} />
      <Animated.View ref={(r) => (refs.current.blockedNative = r)} style={[styles.big, { left: 260, opacity: blockedNative }]} />
      <Animated.View ref={(r) => (refs.current.stop = r)} style={[styles.small, { top: 70, transform: [{ translateX: stopV }] }]} />
      <Animated.View ref={(r) => (refs.current.loop = r)} style={[styles.small, { top: 100, opacity: loopV }]} />
      <Animated.View ref={(r) => (refs.current.width = r)} style={[styles.small, { top: 130, width: widthV }]} />
      <Animated.ScrollView ref={(r) => (refs.current.scroll = r)} style={styles.scroll} onScroll={onScroll} scrollEventThrottle={16}>
        <View style={{ height: 600, backgroundColor: '#202020' }} />
      </Animated.ScrollView>
      <Animated.View
        ref={(r) => (refs.current.header = r)}
        style={[
          styles.header,
          { transform: [{ translateY: scrollY.interpolate({ inputRange: [0, 100], outputRange: [0, -40], extrapolate: 'clamp' }) }] },
        ]}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  root: { width: 400, height: 300, backgroundColor: '#000000' },
  pairs: { position: 'absolute', left: 0, top: 0, width: 180 },
  row: { flexDirection: 'row', height: 20 },
  box: { width: 16, height: 16, marginRight: 4, backgroundColor: '#ffffff' },
  big: { position: 'absolute', top: 0, width: 50, height: 50, backgroundColor: '#ffffff' },
  small: { position: 'absolute', left: 200, width: 20, height: 20, backgroundColor: '#ff8800' },
  scroll: { position: 'absolute', left: 300, top: 150, width: 100, height: 150 },
  header: { position: 'absolute', left: 300, top: 150, width: 100, height: 40, backgroundColor: '#3366ff' },
});

AppRegistry.registerComponent('main', () => App);
