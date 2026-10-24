// End-to-end fixture for tests/native/react_state_styles_e2e_test.cpp:
// hoveredStyle / pressedStyle / focusedStyle / focusVisibleStyle /
// disabledStyle with a transition, group styles and useInteraction values,
// all resolved by the runtime from input (no JS handler, no re-render).
// 400 x 300.
import React, { useEffect, useState } from 'react';
import { AppRegistry, View, Text, Pressable, Animated } from 'react-native';
import { Host, useHostMessage, useInteraction } from '@orktes/jsui-react';

const post = (name, value) => Host.postMessage(name, value);
const reg = {};  // name -> host component (its _nativeTag is the node id)
const at = (name) => (r) => (reg[name] = r);

let renders = 0;
function App() {
  renders++;
  const [tick, setTick] = useState(0);
  useHostMessage('rerender', () => setTick((t) => t + 1));
  useHostMessage('renders', () => post('renders', renders));
  const ix = useInteraction();
  useEffect(() => {
    const ids = {};
    for (const k in reg) ids[k] = reg[k] && reg[k]._nativeTag;
    post('ids', ids);
  }, []);
  return (
    <View style={{ flex: 1 }}>
      <Pressable
        ref={at('card')}
        group
        interaction={ix}
        onPress={() => post('pressed', true)}
        // A new style object each render: the overlays stay on top of it.
        style={{ position: 'absolute', left: 20, top: 20, width: 100, height: 60, backgroundColor: '#202020', borderColor: '#000000', borderWidth: 2, opacity: 1, borderRadius: tick }}
        hoveredStyle={{ backgroundColor: '#404040' }}
        pressedStyle={{ transform: [{ scale: 0.9 }] }}
        focusedStyle={{ borderColor: '#ff0000' }}
        focusVisibleStyle={{ borderColor: '#00ff00', borderWidth: 4 }}
        transition={{ duration: 100, easing: 'linear', pressed: 0 }}
      >
        <Text ref={at('title')} style={{ color: '#808080' }} groupHoveredStyle={{ color: '#ffffff' }}>
          Title
        </Text>
        <View ref={at('dot')} style={{ width: 10, height: 10 }} groupPressedStyle={{ opacity: 0.5 }} groupFocusedStyle={[{ backgroundColor: '#0000ff' }]} />
      </Pressable>
      <Pressable
        ref={at('off')}
        disabled
        style={{ position: 'absolute', left: 20, top: 120, width: 100, height: 40, opacity: 1 }}
        hoveredStyle={{ backgroundColor: '#ff0000' }}
        disabledStyle={{ opacity: 0.4 }}
      />
      <Animated.View
        ref={at('meter')}
        style={{
          position: 'absolute',
          left: 200,
          top: 20,
          width: 10,
          height: 10,
          opacity: ix.hovered,
          transform: [{ translateX: ix.pressed.interpolate({ inputRange: [0, 1], outputRange: [0, 50] }) }],
        }}
      />
    </View>
  );
}

AppRegistry.registerComponent('Main', () => App);
