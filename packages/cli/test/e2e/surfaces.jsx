// End-to-end fixture for tests/native/react_surfaces_e2e_test.cpp: one bundle,
// several root views (jsui_surface_create). `Main` auto-starts on the main
// surface; `Nameplate` instances get their own props, size, input and state;
// `Badge` is content-sized; `Boom` throws when asked to; `Late` registers
// after a timer. Components post
// what they rendered with Host.postMessage.
import React, { useContext, useEffect, useRef, useState } from 'react';
import { AppRegistry, Pressable, RootTagContext, Text, View, useWindowDimensions } from 'react-native';
import { Host } from '@orktes/jsui-react';

function Main() {
  const rootTag = useContext(RootTagContext);
  useEffect(() => Host.postMessage('main', { rootTag }), []);
  return (
    <Pressable style={{ flex: 1, backgroundColor: '#202020' }} onPress={() => Host.postMessage('mainPress', null)}>
      <Text>main</Text>
    </Pressable>
  );
}

function Nameplate({ name, hp, color }) {
  const rootTag = useContext(RootTagContext);
  const { width, height } = useWindowDimensions();
  const mounts = useRef(0);
  const [presses, setPresses] = useState(0);
  useEffect(() => {
    mounts.current++;
    Host.postMessage('mount', { name, rootTag });
    return () => Host.postMessage('unmount', { name });
  }, []);
  useEffect(() => {
    Host.postMessage('plate', { name, hp, rootTag, width, height, presses, mounts: mounts.current });
  });
  return (
    <View style={{ flex: 1, padding: 4 }}>
      <Pressable
        style={{ flex: 1, backgroundColor: color || '#ff0000' }}
        onPress={() => {
          setPresses((p) => p + 1);
          Host.postMessage('press', { name });
        }}
      />
    </View>
  );
}

function Badge({ label }) {
  return (
    <View style={{ width: 64, height: 24, backgroundColor: '#00ff00' }}>
      <Text>{label}</Text>
    </View>
  );
}

function Boom({ boom }) {
  if (boom) throw new Error('boom');
  return <View style={{ flex: 1, backgroundColor: '#ffffff' }} />;
}

function Late() {
  useEffect(() => Host.postMessage('late', null), []);
  return <View style={{ flex: 1, backgroundColor: '#0000ff' }} />;
}

AppRegistry.registerComponent('Main', () => Main);
AppRegistry.registerComponent('Nameplate', () => Nameplate);
AppRegistry.registerComponent('Badge', () => Badge);
AppRegistry.registerComponent('Boom', () => Boom);
setTimeout(() => AppRegistry.registerComponent('Late', () => Late), 500);
