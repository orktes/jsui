// End-to-end fixture for the react_list_e2e test in
// tests/native/react_e2e_test.cpp: a 10,000 row FlatList driven by wheel,
// gamepad focus navigation and host messages through the real runtime.
// Rows are 30 px, blue for even and red for odd indices, so the C++ side
// can check from pixels which rows are on screen.
import React, { useEffect, useRef } from 'react';
import { AppRegistry, View, Text, Pressable, FlatList, StyleSheet } from 'react-native';
import { Host, useHostMessage, bridgeStats } from '@jsui/react';

const DATA = Array.from({ length: 10000 }, (_, i) => ({ id: 'row' + i, n: i }));
let mounted = 0;
let firstViewable = -1;
let lastViewable = -1;

function Row({ item }) {
  useEffect(() => {
    mounted++;
    return () => {
      mounted--;
    };
  }, []);
  return (
    <Pressable
      hasTVPreferredFocus={item.n === 0}
      style={[styles.row, item.n % 2 ? styles.odd : styles.even]}
      onPress={() => Host.postMessage('press', item.n)}
      onFocus={() => Host.postMessage('focus', item.n)}
    >
      <Text style={styles.text}>Row {item.n}</Text>
    </Pressable>
  );
}

const renderItem = ({ item }) => <Row item={item} />;
const onViewableItemsChanged = ({ viewableItems }) => {
  firstViewable = viewableItems.length ? viewableItems[0].index : -1;
  lastViewable = viewableItems.length ? viewableItems[viewableItems.length - 1].index : -1;
};

function App() {
  const list = useRef(null);
  useHostMessage('stats', () => Host.postMessage('stats', { mounted, nodes: bridgeStats().nodes, first: firstViewable, last: lastViewable }));
  useHostMessage('scrollToIndex', (d) => list.current.scrollToIndex({ index: d.index, animated: false, viewPosition: d.viewPosition || 0 }));
  useHostMessage('scrollToEnd', () => list.current.scrollToEnd({ animated: false }));
  return (
    <View style={styles.root}>
      <FlatList
        ref={list}
        data={DATA}
        renderItem={renderItem}
        keyExtractor={(item) => item.id}
        onViewableItemsChanged={onViewableItemsChanged}
        viewabilityConfig={{ itemVisiblePercentThreshold: 100 }}
        onEndReached={() => Host.postMessage('end', DATA.length)}
        onEndReachedThreshold={0.5}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#000000' },
  row: { height: 30, paddingLeft: 40, justifyContent: 'center' },
  even: { backgroundColor: '#0000ff' },
  odd: { backgroundColor: '#ff0000' },
  text: { color: '#ffffff', fontSize: 12 },
});

AppRegistry.registerComponent('main', () => App);
