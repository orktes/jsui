// End-to-end fixture for tests/native/react_vector_e2e_test.cpp:
// react-native-svg and @shopify/react-native-skia (both aliased by the CLI)
// drawn by VectorViews. 400 x 300, white.
// - An <Svg> (0..200 x 0..100, viewBox 0 0 100 50): a red circle whose
//   radius is an Animated value (Animated.createAnimatedComponent(Circle)),
//   driven by a native timing on 'grow'; a group whose opacity runs on the
//   JS driver on 'fade'; an extra blue rect toggled by state on 'toggle'
//   (re-serialization); custom components and hooks inside the Svg.
// - A <Canvas> (0..200 x 100..200): a black stroked Path whose `end` (trim)
//   is an Animated value passed directly, driven natively on 'trim'.
import React, { useEffect, useMemo, useState } from 'react';
import { AppRegistry, View, Animated } from 'react-native';
import { Host, useHostMessage } from '@orktes/jsui-react';
import Svg, { Circle, G, Rect } from 'react-native-svg';
import { Canvas, Path, Group } from '@shopify/react-native-skia';

const post = (name, value) => Host.postMessage(name, value);
const AnimatedCircle = Animated.createAnimatedComponent(Circle);
const reg = {};

// A custom component with a hook inside the Svg.
function Extra({ on }) {
  const [w] = useState(20);
  return on ? <Rect x={70} y={5} width={w} height={10} fill="#0000ff" /> : null;
}

let renders = 0;
function App() {
  renders++;
  const r = useMemo(() => new Animated.Value(5), []);
  const end = useMemo(() => new Animated.Value(0.1), []);
  const opacity = useMemo(() => new Animated.Value(1), []);
  const [extra, setExtra] = useState(false);
  useHostMessage('renders', () => post('renders', renders));
  useHostMessage('grow', () => {
    Animated.timing(r, { toValue: 20, duration: 160, easing: (t) => t, useNativeDriver: true }).start(({ finished }) => post('grown', finished));
  });
  useHostMessage('trim', () => {
    Animated.timing(end, { toValue: 1, duration: 160, easing: (t) => t, useNativeDriver: true }).start(({ finished }) => post('trimmed', finished));
  });
  useHostMessage('fade', () => {
    Animated.timing(opacity, { toValue: 0, duration: 64, useNativeDriver: false }).start(({ finished }) => post('faded', finished));
  });
  useHostMessage('toggle', () => setExtra((x) => !x));
  useEffect(() => {
    post('ids', { svg: reg.svg && reg.svg._nativeTag, canvas: reg.canvas && reg.canvas._nativeTag });
  }, []);
  return (
    <View style={{ flex: 1, backgroundColor: '#fff' }}>
      <Svg ref={(x) => (reg.svg = x)} width={200} height={100} viewBox="0 0 100 50" style={{ position: 'absolute', left: 0, top: 0 }}>
        <AnimatedCircle cx={25} cy={25} r={r} fill="#ff0000" />
        <G opacity={opacity}>
          <Rect x={70} y={30} width={20} height={10} fill="#00ff00" />
        </G>
        <Extra on={extra} />
      </Svg>
      <Canvas ref={(x) => (reg.canvas = x)} style={{ position: 'absolute', left: 0, top: 100, width: 200, height: 100 }}>
        <Group>
          <Path path="M 0 50 L 200 50" style="stroke" strokeWidth={10} color="#000000" end={end} />
        </Group>
      </Canvas>
    </View>
  );
}

AppRegistry.registerComponent('Main', () => App);
