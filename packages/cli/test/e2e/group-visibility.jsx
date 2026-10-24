// End-to-end fixture for tests/native/react_group_visibility_e2e_test.cpp:
// a hover popout built from useGroupState + Visibility, host state bound
// natively (useHostStateBind) into a layout style, Animated.Text and a
// derived Visibility, all without JS once mounted. 400 x 300.
import React, { useEffect, useState } from 'react';
import { AppRegistry, View, Text, Pressable, Animated } from 'react-native';
import { Host, useHostMessage, useGroupState, useHostStateBind, Visibility, Group } from '@orktes/jsui-react';

const post = (name, value) => Host.postMessage(name, value);
const reg = {};
const at = (name) => (r) => (reg[name] = r);

function Tooltip() {
  const g = useGroupState();
  return (
    <Visibility ref={at('tip')} visible={g.instant.hovered} transition={{ duration: 100, easing: 'linear' }} style={{ position: 'absolute', left: 110, top: 0, width: 80, height: 40 }}>
      <Text>Sword of Dawn</Text>
    </Visibility>
  );
}

function Named() {
  // The outer group by name, from inside an inner one.
  const outer = useGroupState('slot');
  return <Animated.View ref={at('named')} style={{ width: 4, height: 4, opacity: outer.instant.pressed }} />;
}

function Health() {
  const hp = useHostStateBind('player', 'health.current');
  const name = useHostStateBind('player', ['name']);
  const low = React.useMemo(() => Animated.derive([hp], (h) => h > 0 && h < 30), [hp]);
  return (
    <View style={{ position: 'absolute', left: 20, top: 200 }}>
      <Animated.View ref={at('bar')} style={{ height: 6, width: hp }} />
      <Animated.Text ref={at('name')} text={name} />
      <Visibility ref={at('low')} visible={low}>
        <Text>LOW</Text>
      </Visibility>
    </View>
  );
}

let renders = 0;
function App() {
  renders++;
  const [shown, setShown] = useState(false);
  useHostMessage('show', () => setShown(true));
  useHostMessage('renders', () => post('renders', renders));
  useEffect(() => {
    const ids = {};
    for (const k in reg) ids[k] = reg[k] && reg[k]._nativeTag;
    post('ids', ids);
  }, []);
  return (
    <View style={{ flex: 1 }}>
      <Group name="slot" ref={at('slot')} style={{ position: 'absolute', left: 20, top: 20, width: 100, height: 60 }}>
        <Pressable group ref={at('card')} style={{ width: 100, height: 60 }} onPress={() => {}}>
          <Tooltip />
          <Named />
        </Pressable>
      </Group>
      <Visibility ref={at('static')} visible={shown} mode="hide" style={{ position: 'absolute', left: 300, top: 20, width: 20, height: 20 }} />
      <Health />
    </View>
  );
}

AppRegistry.registerComponent('Main', () => App);
