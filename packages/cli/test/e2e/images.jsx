// End-to-end fixture for tests/native/react_images_e2e_test.cpp: React
// Native's Image events (onLoadStart, onProgress, onLoad with
// nativeEvent.source {uri, width, height}, onError, onLoadEnd) for data URIs,
// require()'d bundled assets, URLs served by the embedder's loader, missing
// files and images already cached at mount. Each Image logs its events as
// host messages; the host shows a list of probes with 'show'. 400 x 300.
import React, { useEffect, useState } from 'react';
import { AppRegistry, View, Image, ImageBackground, StyleSheet } from 'react-native';
import { Host, useHostMessage } from '@orktes/jsui-react';

const DOT = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAADklEQVR4nGP4z8AAQv8BD/kD/YURmXYAAAAASUVORK5CYII=';
const SWORD = require('../../../../examples/assets/item-sword.png');
const SOURCES = { dot: DOT, sword: SWORD, bad: 'data:image/png;base64,AAAA' };

function Probe({ name, src, background }) {
  const log = (what, extra) => Host.postMessage('img', Object.assign({ name, what }, extra));
  const props = {
    source: SOURCES[src] || { uri: src },
    style: styles.img,
    onLoadStart: () => log('start'),
    onProgress: (e) => log('progress', { loaded: e.nativeEvent.loaded, total: e.nativeEvent.total }),
    onLoad: (e) => {
      const s = e.nativeEvent.source;
      const uri = typeof s.uri !== 'string' ? null : s.uri.startsWith('data:') ? 'data:' : s.uri.slice(s.uri.lastIndexOf('/') + 1);
      log('load', { width: s.width, height: s.height, uri });
    },
    onError: (e) => log('error', { error: typeof e.nativeEvent.error === 'string' && e.nativeEvent.error.length > 0 }),
    onLoadEnd: () => log('end'),
  };
  // Passive effects run before the image's events (they are asynchronous).
  useEffect(() => log('effect'), []);
  return background ? <ImageBackground {...props} /> : <Image {...props} />;
}

function App() {
  const [items, setItems] = useState([]);
  useHostMessage('show', (list) => setItems(list));
  useEffect(() => Host.postMessage('ready', true), []);
  return (
    <View style={styles.root}>
      {items.map((it) => (
        <Probe key={it.key || it.name} name={it.name} src={it.src} background={it.background} />
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  root: { width: 400, height: 300, flexDirection: 'row', flexWrap: 'wrap', backgroundColor: '#000000' },
  img: { width: 20, height: 20 },
});

AppRegistry.registerComponent('main', () => App);
