// End-to-end fixture for tests/native/react_host_buffers_e2e_test.cpp: frame
// data (jsui_set_buffer) driving markers. The marker list re-renders only
// when the number of enemies changes (useHostBufferSelector); each marker
// follows its enemy through Animated.hostBuffer nodes (useHostBufferValue),
// which the runtime evaluates natively: no React render per frame. A `Plate`
// surface of the same runtime follows the same buffer. 400 x 300.
import React from 'react';
import { AppRegistry, View, Animated } from 'react-native';
import { Host, useHostBuffer, useHostBufferSelector, useHostBufferValue } from '@orktes/jsui-react';

const STRIDE = 3; // x, y, hp

function Marker({ i, id }) {
  const x = useHostBufferValue('enemies', i * STRIDE);
  const y = useHostBufferValue('enemies', i * STRIDE + 1);
  const hp = useHostBufferValue('enemies', i * STRIDE + 2);
  const scaleX = hp.interpolate({ inputRange: [0, 100], outputRange: [0, 1], extrapolate: 'clamp' });
  return (
    <Animated.View
      nativeID={id}
      style={{
        position: 'absolute',
        width: 40,
        height: 8,
        backgroundColor: '#e33',
        transform: [{ translateX: x }, { translateY: y }, { scaleX }],
      }}
    />
  );
}

function Markers() {
  const count = useHostBufferSelector('enemies', (b) => Math.floor(b.length / STRIDE), 0);
  Host.postMessage('render', count);
  return (
    <View style={{ flex: 1 }}>
      {Array.from({ length: count }, (_, i) => (
        <Marker key={i} i={i} id={'m' + i} />
      ))}
    </View>
  );
}

function Score() {
  const s = useHostBuffer('score');
  Host.postMessage('score', s ? Array.from(s) : null);
  return null;
}

function Plate({ i }) {
  return (
    <View style={{ flex: 1 }}>
      <Marker i={i} id="plate" />
    </View>
  );
}

AppRegistry.registerComponent('Main', () => () => (
  <View style={{ flex: 1 }}>
    <Markers />
    <Score />
  </View>
));
AppRegistry.registerComponent('Plate', () => Plate);
