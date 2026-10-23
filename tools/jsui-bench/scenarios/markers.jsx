// jsui-bench "markers": N enemy markers (a nameplate-like box with an HP bar)
// that move every frame, fed three ways (--markers N,json|buffer|native):
//   JsonMarkers    jsui_set_state('enemies', '[[x,y,hp],...]') + useHostState:
//                  JSON built by the host, parsed natively and in JS, every
//                  marker re-rendered by React every frame
//   BufferMarkers  jsui_set_buffer('enemies', Float32Array x,y,hp...) +
//                  useHostBuffer: no JSON, but still a React render per frame
//   NativeMarkers  the same buffer through Animated.hostBuffer
//                  (useHostBufferValue): the runtime moves the markers, no JS
//                  per frame; React renders only when the count changes
import React, { memo } from 'react';
import { AppRegistry, Animated, StyleSheet, View } from 'react-native';
import { useHostBuffer, useHostBufferSelector, useHostBufferValue, useHostState } from '@orktes/jsui-react';

const Marker = memo(function Marker({ x, y, hp }) {
  return (
    <View style={[styles.marker, { transform: [{ translateX: x }, { translateY: y }] }]}>
      <View style={styles.bar}>
        <View style={[styles.fill, { transform: [{ scaleX: hp / 100 }] }]} />
      </View>
    </View>
  );
});

function JsonMarkers() {
  const list = useHostState('enemies', []);
  return (
    <View style={styles.root}>
      {list.map((e, i) => (
        <Marker key={i} x={e[0]} y={e[1]} hp={e[2]} />
      ))}
    </View>
  );
}

function BufferMarkers() {
  const b = useHostBuffer('enemies');
  const n = b ? Math.floor(b.length / 3) : 0;
  const out = [];
  for (let i = 0; i < n; i++) out.push(<Marker key={i} x={b[i * 3]} y={b[i * 3 + 1]} hp={b[i * 3 + 2]} />);
  return <View style={styles.root}>{out}</View>;
}

function NativeMarker({ i }) {
  const x = useHostBufferValue('enemies', i * 3);
  const y = useHostBufferValue('enemies', i * 3 + 1);
  const hp = useHostBufferValue('enemies', i * 3 + 2);
  const scaleX = hp.interpolate({ inputRange: [0, 100], outputRange: [0, 1] });
  return (
    <Animated.View style={[styles.marker, { transform: [{ translateX: x }, { translateY: y }] }]}>
      <View style={styles.bar}>
        <Animated.View style={[styles.fill, { transform: [{ scaleX }] }]} />
      </View>
    </Animated.View>
  );
}

function NativeMarkers() {
  const n = useHostBufferSelector('enemies', (b) => Math.floor(b.length / 3), 0);
  const out = [];
  for (let i = 0; i < n; i++) out.push(<NativeMarker key={i} i={i} />);
  return <View style={styles.root}>{out}</View>;
}

AppRegistry.registerComponent('JsonMarkers', () => JsonMarkers);
AppRegistry.registerComponent('BufferMarkers', () => BufferMarkers);
AppRegistry.registerComponent('NativeMarkers', () => NativeMarkers);

const styles = StyleSheet.create({
  root: { flex: 1 },
  marker: { position: 'absolute', left: 0, top: 0, width: 48, height: 14, padding: 3, borderRadius: 3, backgroundColor: '#0d1322cc' },
  bar: { flex: 1, borderRadius: 2, backgroundColor: '#3a1d24', overflow: 'hidden' },
  fill: { flex: 1, backgroundColor: '#ef476f' },
});
