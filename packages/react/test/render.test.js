import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, useState, Fragment, Suspense } from 'react';
import { View, Text, Image, StyleSheet, render, ScrollView, FlatList, AppRegistry, flushSync } from '../src/index.js';

const styles = StyleSheet.create({ box: { width: 10, height: 20, backgroundColor: 'red' }, big: { width: 30 } });

test('renders a tree with flattened styles and raw text', () => {
  render(h(View, { style: [styles.box, styles.big, null, false] }, h(Text, { numberOfLines: 2 }, 'Hello ', 'world')));
  const tree = mock.tree();
  assert.equal(tree[0], 'Root');
  const [type, props, text] = tree[2];
  assert.equal(type, 'View');
  assert.deepEqual(props.style, { width: 30, height: 20, backgroundColor: 'red' });
  assert.equal(text[0], 'Text');
  assert.equal(text[1].numberOfLines, 2);
  assert.deepEqual(text.slice(2), ['Hello ', 'world']);
  render(null);
  assert.equal(mock.root().children.length, 0);
});

test('string outside <Text> throws like React Native', async () => {
  const errors = [];
  const uncaught = [];
  globalThis.__jsui_reportError = (e) => uncaught.push(e);
  const origError = console.error;
  console.error = (...a) => errors.push(a.join(' '));
  try {
    assert.throws(() => render(h(View, null, 'oops')), /Text strings must be rendered within a <Text> component/);
  } catch (e) {
    // React reports uncaught render errors via onUncaughtError (logged) instead of throwing.
    assert.ok(errors.some((m) => m.includes('Text strings must be rendered within a <Text> component')), String(e));
  } finally {
    console.error = origError;
  }
  // The error is also rethrown asynchronously so the runtime records it.
  delete globalThis.__jsui_reportError;
  assert.equal(uncaught.length, 1);
  assert.match(String(uncaught[0]), /Text strings/);
  render(null);
});

test('setProps is only sent when native-visible props change', () => {
  let set;
  function App() {
    const [n, setN] = useState(0);
    set = setN;
    // New style array + new handler each render, same values.
    return h(View, { style: [styles.box, { opacity: n > 1 ? 0.5 : 1 }], onLayout: () => {} });
  }
  render(h(App));
  const id = mock.root().children[0];
  assert.deepEqual(mock.nodes.get(id).props.__events, ['layout']);
  mock.reset();
  flushSync(() => set(1));
  assert.equal(mock.count('setProps'), 0, 'identical props must not cross the bridge');
  flushSync(() => set(2));
  assert.equal(mock.count('setProps'), 1);
  assert.equal(mock.nodes.get(id).props.style.opacity, 0.5);
  render(null);
});

test('keyed reorder uses insertBefore and removal destroys native nodes', () => {
  let set;
  function List() {
    const [items, setItems] = useState(['a', 'b', 'c']);
    set = setItems;
    return h(View, null, items.map((k) => h(View, { key: k, nativeID: k })));
  }
  render(h(List));
  const parent = mock.nodes.get(mock.root().children[0]);
  const ids = () => parent.children.map((c) => mock.nodes.get(c).props.nativeID);
  assert.deepEqual(ids(), ['a', 'b', 'c']);
  flushSync(() => set(['c', 'a']));
  assert.deepEqual(ids(), ['c', 'a']);
  assert.ok(mock.count('destroyNode') >= 1);
  const before = mock.nodes.size;
  render(null);
  assert.ok(mock.nodes.size < before);
  assert.equal(mock.nodes.size, 1, 'only the root remains');
});

test('Image normalizes sources and applies registered asset size', () => {
  globalThis.__jsui_assets = { 'assets/logo-abc.png': { width: 64, height: 32 } };
  render(h(Fragment, null, h(Image, { source: 'assets/logo-abc.png' }), h(Image, { source: { uri: 'http://x/y.png' }, style: { width: 5, height: 6 }, resizeMode: 'cover' })));
  const [a, b] = mock.root().children.map((id) => mock.nodes.get(id).props);
  assert.deepEqual(a.source, { uri: 'assets/logo-abc.png', width: 64, height: 32 });
  assert.equal(a.style.width, 64);
  assert.equal(a.style.height, 32);
  assert.equal(b.source.uri, 'http://x/y.png');
  assert.equal(b.style.width, 5);
  assert.equal(b.resizeMode, 'cover');
  assert.equal(b.style.resizeMode, 'cover');
  render(null);
});

test('ScrollView wraps content and exposes scrollTo through ref', () => {
  const ref = { current: null };
  render(h(ScrollView, { ref, horizontal: true, contentContainerStyle: { padding: 4 } }, h(View)));
  const sv = mock.nodes.get(mock.root().children[0]);
  assert.equal(sv.type, 'ScrollView');
  assert.equal(sv.props.horizontal, true);
  assert.equal(sv.props.style.flexDirection, 'row');
  const content = mock.nodes.get(sv.children[0]);
  assert.deepEqual(content.props.style, { flexDirection: 'row', padding: 4 });
  mock.reset();
  ref.current.scrollTo({ x: 40, y: 0 });
  ref.current.scrollTo(10);
  assert.deepEqual(mock.calls[0], ['scrollTo', sv.id, 40, 0, true]);
  assert.deepEqual(mock.calls[1], ['scrollTo', sv.id, 0, 10, true]);
  render(null);
});

test('FlatList renders items, separators, header/footer/empty and numColumns', () => {
  const data = [{ id: 'x' }, { id: 'y' }, { id: 'z' }];
  render(
    h(FlatList, {
      data,
      renderItem: ({ item, index }) => h(Text, null, item.id + index),
      ItemSeparatorComponent: () => h(View, { nativeID: 'sep' }),
      ListHeaderComponent: h(Text, null, 'head'),
      ListFooterComponent: () => h(Text, null, 'foot'),
    }),
  );
  const all = mock.text(mock.root().children[0]);
  assert.equal(all, 'headx0y1z2foot');
  assert.equal(mock.findAll((n) => n.props.nativeID === 'sep').length, 2);
  render(h(FlatList, { data: [], renderItem: () => null, ListEmptyComponent: () => h(Text, null, 'empty') }));
  assert.equal(mock.text(mock.root().children[0]), 'empty');
  render(h(FlatList, { data: [1, 2, 3, 4, 5], numColumns: 2, keyExtractor: (i) => String(i), renderItem: ({ item }) => h(Text, null, String(item)) }));
  const rows = mock.findAll((n) => n.type === 'View' && n.props.style && n.props.style.flexDirection === 'row');
  assert.equal(rows.length, 3);
  assert.equal(mock.text(mock.root().children[0]), '12345');
  render(null);
});

test('Suspense hides and unhides with display none', async () => {
  let resolve;
  const promise = new Promise((r) => (resolve = r));
  let done = false;
  function Lazy() {
    if (!done) throw promise;
    return h(Text, null, 'loaded');
  }
  render(h(View, null, h(Suspense, { fallback: h(Text, null, 'loading') }, h(Lazy))));
  assert.equal(mock.text(mock.root().children[0]), 'loading');
  done = true;
  resolve();
  await new Promise((r) => setTimeout(r, 400));
  assert.equal(mock.text(mock.root().children[0]), 'loaded');
  render(null);
});

test('AppRegistry.registerComponent auto-starts the app', async () => {
  function Main() {
    return h(View, { nativeID: 'main' });
  }
  AppRegistry.registerComponent('main', () => Main);
  await Promise.resolve();
  await Promise.resolve();
  const root = mock.nodes.get(mock.root().children[0]);
  assert.deepEqual(root.props.style, { flex: 1, pointerEvents: 'box-none' });
  assert.equal(mock.nodes.get(root.children[0]).props.nativeID, 'main');
  render(null);
});
