// End-to-end fixture for tests/native/react_js_thread_e2e_test.cpp: UI that
// must stay correct every frame while JS is stalled (JS on its own thread).
// - 32 markers follow host frame data ('pos': one x per marker) through
//   Animated.hostBuffer nodes: marker 0 also through an interpolation, marker
//   1 through an Animated.derive expression. The runtime evaluates them on
//   the owner thread, so they follow while JS is busy.
// - A ScrollView for a fling that continues while JS is busy.
// - Host.handle('stall', { ms }): busy JS for `ms` (wall time; jsui_call_ui).
// - 'garbage': allocates and drops a large object graph (GC work).
// - 'label' (state): a Text showing host state (jsui_set_state + sync tick).
// - 'pair': a commit that inserts two views.
// 400 x 300.
import React, { useEffect, useMemo, useState } from 'react';
import { AppRegistry, View, Animated, ScrollView, Text } from 'react-native';
import { Host, useHostBufferValue, useHostMessage, useHostState } from '@orktes/jsui-react';

const COUNT = 32;

function Marker({ i }) {
  const x = useHostBufferValue('pos', i);
  const style = useMemo(() => {
    if (i === 0) return { translateX: x, translateY: x.interpolate({ inputRange: [0, 300], outputRange: [0, 150] }) };
    if (i === 1) return { translateX: Animated.derive([x], (v) => v * 2 + 1), translateY: 0 };
    return { translateX: x, translateY: 0 };
  }, [x, i]);
  return (
    <Animated.View
      nativeID={'m' + i}
      style={{
        position: 'absolute',
        top: i * 4,
        width: 6,
        height: 3,
        backgroundColor: '#e33',
        transform: [{ translateX: style.translateX }, { translateY: style.translateY }],
      }}
    />
  );
}

function Label() {
  const label = useHostState('label', '');
  return (
    <Text nativeID="label" style={{ position: 'absolute', left: 0, top: 280, fontSize: 12 }}>
      {String(label)}
    </Text>
  );
}

// 'pair': one more pair of views in one commit (never seen half applied).
function Pairs() {
  const [n, setN] = useState(0);
  useHostMessage('pair', () => setN((k) => k + 1));
  return (
    <View style={{ position: 'absolute', left: 0, top: 150, width: 100, height: 100 }}>
      {Array.from({ length: n }, (_, k) => (
        <React.Fragment key={k}>
          <View nativeID={'pa' + k} style={{ width: 2, height: 1 }} />
          <View nativeID={'pb' + k} style={{ width: 2, height: 1 }} />
        </React.Fragment>
      ))}
    </View>
  );
}

function App() {
  useEffect(() => {
    Host.handle('stall', ({ ms }) => {
      const end = Date.now() + ms;
      let n = 0;
      while (Date.now() < end) n++;
      return n > 0;
    });
    Host.postMessage('ready', true);
  }, []);
  useHostMessage('garbage', () => {
    // Cycles: reference counting cannot free them, the collector must.
    const nodes = [];
    for (let i = 0; i < 200000; i++) nodes.push({ i, all: nodes, s: 'n' + i });
  });
  return (
    <View style={{ flex: 1 }}>
      <ScrollView nativeID="list" style={{ position: 'absolute', left: 200, top: 160, width: 200, height: 110 }}>
        <View style={{ height: 6000, backgroundColor: '#336699' }} />
      </ScrollView>
      {Array.from({ length: COUNT }, (_, i) => (
        <Marker key={i} i={i} />
      ))}
      <Label />
      <Pairs />
    </View>
  );
}

AppRegistry.registerComponent('Main', () => App);
