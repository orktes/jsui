// End-to-end fixture for tests/native/react_scroll_e2e_test.cpp: native
// touch / wheel / gamepad scrolling -> @orktes/jsui-react ScrollView events -> host
// messages back to C.
import React, { useRef } from 'react';
import { AppRegistry, View, Text, ScrollView, PanResponder } from 'react-native';
import { Host, useHostMessage } from '@orktes/jsui-react';

const rows = Array.from({ length: 20 }, (_, i) => i);

// Gesture negotiation below the list: a PanResponder (blocks native gestures
// by default) and a responder that lets the ScrollView take over.
const pan = PanResponder.create({
  onStartShouldSetPanResponder: () => true,
  onPanResponderRelease: (e, g) => Host.postMessage('pan', Math.round(g.dy)),
  onPanResponderTerminate: () => Host.postMessage('panterm', 0),
});
const polite = PanResponder.create({
  onStartShouldSetPanResponder: () => true,
  onShouldBlockNativeResponder: () => false,
  onPanResponderTerminate: () => Host.postMessage('politeterm', 0),
});

function App() {
  const ref = useRef(null);
  useHostMessage('scrollTo', (y) => ref.current.scrollTo({ y, animated: true }));
  useHostMessage('scrollToEnd', () => ref.current.scrollToEnd({ animated: false }));
  const post = (name) => (e) => Host.postMessage(name, Math.round(e.nativeEvent.contentOffset.y));
  return (
    <View style={{ width: 200, height: 300, backgroundColor: '#123456' }}>
      <ScrollView
        ref={ref}
        style={{ height: 100, flexGrow: 0 }}
        contentOffset={{ x: 0, y: 50 }}
        scrollEventThrottle={100}
        stickyHeaderIndices={[0]}
        onScroll={post('scroll')}
        onScrollBeginDrag={post('begin')}
        onScrollEndDrag={(e) => Host.postMessage('end', [Math.round(e.nativeEvent.contentOffset.y), e.nativeEvent.velocity.y])}
        onMomentumScrollBegin={post('mbegin')}
        onMomentumScrollEnd={post('mend')}
      >
        <Text style={{ height: 20 }}>Header</Text>
        {rows.map((i) => (
          <View key={i} style={{ height: 50, backgroundColor: i % 2 ? '#334466' : '#446688' }} />
        ))}
      </ScrollView>
      <ScrollView style={{ height: 200, flexGrow: 0 }} onScrollBeginDrag={(e) => Host.postMessage('outerbegin', Math.round(e.nativeEvent.contentOffset.y))}>
        <View {...pan.panHandlers} style={{ height: 100 }} />
        <View {...polite.panHandlers} style={{ height: 100 }} />
        <View style={{ height: 400 }} />
      </ScrollView>
    </View>
  );
}

AppRegistry.registerComponent('main', () => App);
