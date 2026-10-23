// End-to-end fixture for tests/native/react_e2e_test.cpp: real native input
// -> @orktes/jsui-react events -> React state -> host messages back to C.
import React, { useEffect, useRef, useState } from 'react';
import { AppRegistry, View, Text, Pressable, TextInput, Animated, StyleSheet, useWindowDimensions } from 'react-native';
import { Host, useHostMessage } from '@orktes/jsui-react';

function App() {
  const [text, setText] = useState('');
  const fade = useRef(new Animated.Value(0)).current;
  useHostMessage('ping', (data) => Host.postMessage('pong', data));
  const { width, height, scale } = useWindowDimensions();
  useEffect(() => Host.postMessage('dims', [width, height, scale]), [width, height, scale]);
  useEffect(() => {
    Animated.timing(fade, { toValue: 1, duration: 100, useNativeDriver: true }).start((r) => Host.postMessage('anim', r));
  }, [fade]);
  return (
    <View style={styles.root}>
      <Pressable hasTVPreferredFocus style={styles.button} onPress={(e) => Host.postMessage('press', 'A:' + e.nativeEvent.pointerType)}>
        <Text>A</Text>
      </Pressable>
      <Pressable style={styles.button} onPress={() => Host.postMessage('press', 'B')} onLongPress={() => Host.postMessage('long', 'B')}>
        <Text>B</Text>
      </Pressable>
      <TextInput
        style={styles.input}
        value={text}
        onChangeText={(t) => {
          setText(t);
          Host.postMessage('text', t);
        }}
        onSubmitEditing={(e) => Host.postMessage('submit', e.nativeEvent.text)}
      />
      <Animated.View style={[styles.fade, { opacity: fade }]} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: { width: 200, height: 300, backgroundColor: '#123456' },
  button: { width: 100, height: 50, backgroundColor: '#ff0000' },
  input: { width: 100, height: 30, backgroundColor: '#00ff00' },
  fade: { width: 10, height: 10, backgroundColor: '#0000ff' },
});

AppRegistry.registerComponent('main', () => App);
