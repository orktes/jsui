// End-to-end fixture for tests/native/react_interactions_e2e_test.cpp:
// hover (onHoverIn / onHoverOut, delayHoverIn, the hovered style state),
// press timing, an endless Animated loop (jsui_wants_frame without input)
// and LayoutAnimation create / update / delete, checked from host messages
// and rendered pixels. 400 x 300.
import React, { useEffect, useRef, useState } from 'react';
import { AppRegistry, View, Pressable, Animated, Easing, LayoutAnimation, StyleSheet, findNodeHandle, UIManager } from 'react-native';
import { Host, useHostMessage } from '@jsui/react';

function App() {
  const pulse = useRef(new Animated.Value(0)).current;
  const loop = useRef(null);
  const [wide, setWide] = useState(false);
  const [extra, setExtra] = useState(false);
  const [gone, setGone] = useState(false);
  const growRef = useRef(null);

  useHostMessage('loop', () => {
    loop.current = Animated.loop(Animated.timing(pulse, { toValue: 1, duration: 400, easing: Easing.linear, useNativeDriver: true }));
    loop.current.start();
  });
  useHostMessage('stop', () => loop.current && loop.current.stop());
  useHostMessage('grow', () => {
    LayoutAnimation.configureNext({ duration: 200, update: { type: 'linear' } }, () => Host.postMessage('growDone', true));
    setWide(true);
  });
  useHostMessage('add', () => {
    LayoutAnimation.configureNext({ duration: 200, create: { type: 'linear', property: 'opacity' } });
    setExtra(true);
  });
  useHostMessage('remove', () => {
    LayoutAnimation.configureNext({ duration: 200, delete: { type: 'linear', property: 'scaleXY' } });
    setGone(true);
  });
  useHostMessage('measure', () => {
    UIManager.measure(findNodeHandle(growRef.current), (x, y, w) => Host.postMessage('width', w));
  });
  useEffect(() => Host.postMessage('ready', true), []);

  return (
    <View style={styles.root}>
      <Pressable
        style={({ hovered, pressed }) => [styles.hover, { backgroundColor: pressed ? '#0000ff' : hovered ? '#00ff00' : '#ff0000' }]}
        onHoverIn={() => Host.postMessage('hover', 'in')}
        onHoverOut={() => Host.postMessage('hover', 'out')}
        onPressIn={() => Host.postMessage('pressIn', 'H')}
        onPressOut={() => Host.postMessage('pressOut', 'H')}
        onPress={() => Host.postMessage('press', 'H')}
      />
      <Pressable style={styles.delayed} delayHoverIn={100} delayHoverOut={50} onHoverIn={() => Host.postMessage('dhover', 'in')} onHoverOut={() => Host.postMessage('dhover', 'out')} />
      <Animated.View style={[styles.pulse, { opacity: pulse }]} />
      <View ref={growRef} style={[styles.grow, { width: wide ? 150 : 50 }]} />
      {extra ? <View style={styles.extra} /> : null}
      {gone ? null : <View style={styles.doomed} />}
    </View>
  );
}

const styles = StyleSheet.create({
  root: { width: 400, height: 300, backgroundColor: '#000000' },
  hover: { position: 'absolute', left: 0, top: 0, width: 100, height: 50 },
  delayed: { position: 'absolute', left: 0, top: 60, width: 100, height: 40, backgroundColor: '#888888' },
  pulse: { position: 'absolute', left: 0, top: 120, width: 20, height: 20, backgroundColor: '#ffffff' },
  grow: { position: 'absolute', left: 200, top: 0, height: 50, backgroundColor: '#ffff00' },
  extra: { position: 'absolute', left: 200, top: 100, width: 50, height: 50, backgroundColor: '#00ffff' },
  doomed: { position: 'absolute', left: 300, top: 200, width: 60, height: 60, backgroundColor: '#ff00ff' },
});

AppRegistry.registerComponent('main', () => App);
