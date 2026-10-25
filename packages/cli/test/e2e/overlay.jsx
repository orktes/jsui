// End-to-end fixture for tests/native/react_overlay_e2e_test.cpp: hover
// popouts and badges in the overlay layer (<Overlay>), placed natively
// against their anchors (scrolling, flips) and kept inside the anchor's
// hover group. 400 x 300.
import React, { useEffect, useRef } from 'react';
import { AppRegistry, View, Text, Pressable, ScrollView } from 'react-native';
import { Host, useHostMessage, useGroupState, Visibility, Overlay, OverlayArrow } from '@orktes/jsui-react';

const post = (name, value) => Host.postMessage(name, value);
const reg = {};
const at = (name) => (r) => (reg[name] = r);

// A hover popout that takes clicks: rendered from inside the slot's group,
// so the pointer on it keeps the slot hovered.
function Popout() {
  const g = useGroupState();
  return (
    <Overlay placement="right">
      <Visibility ref={at('pop')} visible={g.instant.hovered}>
        <OverlayArrow ref={at('arrow')} style={{ width: 10, height: 10, backgroundColor: '#333', transform: [{ rotate: '45deg' }] }} />
        <Pressable ref={at('popBtn')} onPress={() => post('popPress', 1)} style={{ width: 120, height: 50, backgroundColor: '#222' }}>
          <Text>Sword of Dawn</Text>
        </Pressable>
      </Visibility>
    </Overlay>
  );
}

let renders = 0;
function App() {
  renders++;
  const low = useRef(null);
  useHostMessage('renders', () => post('renders', renders));
  useEffect(() => {
    const ids = {};
    for (const k in reg) ids[k] = reg[k] && reg[k]._nativeTag;
    post('ids', ids);
  }, []);
  return (
    <View style={{ flex: 1 }}>
      <ScrollView ref={at('list')} style={{ position: 'absolute', left: 20, top: 20, width: 200, height: 150 }}>
        <View style={{ height: 40 }} />
        <Pressable group ref={at('slot')} onPress={() => post('slotPress', 1)} style={{ width: 180, height: 30, backgroundColor: '#456' }}>
          <Popout />
          {/* Always shown: follows the row while the list scrolls. */}
          <Overlay placement="bottom-start" offset={2}>
            <View ref={at('badge')} pointerEvents="none" style={{ width: 30, height: 12, backgroundColor: '#f2c35b' }} />
          </Overlay>
        </Pressable>
        <View style={{ height: 600 }} />
      </ScrollView>
      <View ref={low} style={{ position: 'absolute', left: 300, top: 270, width: 40, height: 20, backgroundColor: '#654' }} />
      {/* Anchored by ref, asks for bottom: no room, flips to the top. */}
      <Overlay anchor={low} placement="bottom" offset={4}>
        <View ref={at('flip')} style={{ width: 60, height: 40, backgroundColor: '#789' }} />
      </Overlay>
    </View>
  );
}

AppRegistry.registerComponent('Main', () => App);
