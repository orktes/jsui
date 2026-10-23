import React, { useEffect, useState } from 'react';
import { Text, View } from 'react-native';
import { Host, useHostMessage } from '@orktes/jsui-react';
import { label } from './label';

export default function App() {
  const [n, setN] = useState(0);
  useHostMessage('bump', () => setN((x) => x + 1));
  const text = 'v1 ' + label + ' ' + n;
  useEffect(() => Host.postMessage('render', text));
  return (
    <View style={{ flex: 1, backgroundColor: '#203040' }}>
      <Text style={{ color: 'white' }}>{text}</Text>
    </View>
  );
}
