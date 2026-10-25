// End-to-end fixture for tests/native/react_reveal_e2e_test.cpp: a native
// typewriter (Animated.Text reveal={value} over nested spans) and
// controllable clocks (Animated.clock({...})). 400 x 300.
// - 'type': a native timing reveals "Hello world!" (the "world" a red span)
//   one grapheme per 16 ms frame; the host checks the reveal and the pixels
//   every frame with jsui_tick_native (no JS) and that nothing re-rendered.
// - A clock box (translateX: seconds) the host pauses / resumes / resets /
//   speeds up through messages; a second clock held by host state
//   ('game'.running) through useHostStateBind.
import React, { useEffect, useMemo } from 'react';
import { AppRegistry, View, Animated, Text } from 'react-native';
import { Host, useHostMessage, useHostStateBind } from '@orktes/jsui-react';

const post = (name, value) => Host.postMessage(name, value);
const reg = {};

let renders = 0;
function App() {
  renders++;
  useHostMessage('renders', () => post('renders', renders));
  const count = useMemo(() => new Animated.Value(0, { useNativeDriver: true }), []);
  const fade = useMemo(() => new Animated.Value(0, { useNativeDriver: true }), []);
  const clock = useMemo(() => Animated.clock({ paused: true }), []);
  const running = useHostStateBind('game', 'running');
  const gated = useMemo(() => Animated.clock({ running }), [running]);
  useHostMessage('type', () => {
    Animated.timing(count, { toValue: 12, duration: 12 * 16, easing: (t) => t, useNativeDriver: true }).start(({ finished }) => post('typed', finished));
  });
  useHostMessage('fade', (v) => fade.setValue(v));
  useHostMessage('clock', (cmd) => {
    if (cmd.op === 'pause') clock.pause();
    else if (cmd.op === 'resume') clock.resume();
    else if (cmd.op === 'reset') clock.reset(cmd.t);
    else if (cmd.op === 'speed') clock.setSpeed(cmd.speed);
    post('clockPaused', clock.isPaused());
  });
  useEffect(() => {
    const ids = {};
    for (const k in reg) ids[k] = reg[k] && reg[k]._nativeTag;
    post('ids', ids);
  }, []);
  return (
    <View style={{ flex: 1, backgroundColor: '#000' }}>
      <Animated.Text ref={(r) => (reg.line = r)} reveal={count} style={{ position: 'absolute', left: 10, top: 10, fontSize: 32, color: '#fff' }}>
        Hello <Text style={{ color: '#f00' }}>world</Text>!
      </Animated.Text>
      <Animated.Text ref={(r) => (reg.words = r)} reveal={fade} revealMode="words" revealFade={1} style={{ position: 'absolute', left: 10, top: 60, fontSize: 32, color: '#fff' }}>
        one two three
      </Animated.Text>
      <Animated.View ref={(r) => (reg.clock = r)} style={{ position: 'absolute', top: 200, width: 4, height: 4, transform: [{ translateX: clock }] }} />
      <Animated.View ref={(r) => (reg.gated = r)} style={{ position: 'absolute', top: 220, width: 4, height: 4, transform: [{ translateX: gated }] }} />
    </View>
  );
}

AppRegistry.registerComponent('Main', () => App);
