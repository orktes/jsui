// requireNativeComponent / codegenNativeCommands against the mock bridge:
// what reaches native (the NativeComponentView host node, the class name,
// plain props, Animated props as prop:<name> style keys also with the
// native driver, the custom event list), commands, and custom events.
// Drawing itself is native: tests/native/native_component_test.cpp.
import { mock, animated } from './mock-native-animated.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, createRef } from 'react';
import {
  Animated,
  UIManager,
  codegenNativeCommands,
  codegenNativeComponent,
  hasNativeComponent,
  render,
  requireNativeComponent,
} from '../src/index.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const byId = (id) => mock.find((x) => x.props.nativeID === id);

test('the host node carries the class name and the plain props', async () => {
  const Radar = requireNativeComponent('Radar');
  assert.equal(requireNativeComponent('Radar'), Radar, 'one component per name');
  assert.equal(codegenNativeComponent('Radar'), Radar);
  assert.equal(Radar.displayName, 'Radar');
  render(
    h(Radar, { nativeID: 'r', range: 120, contacts: [{ x: 1, y: 2 }], tint: '#0f0', onPress: () => {}, style: { width: 10 } }),
  );
  await wait();
  const n = byId('r');
  assert.equal(n.type, 'NativeComponentView');
  assert.equal(n.props.component, 'Radar');
  assert.equal(n.props.range, 120);
  assert.deepEqual(n.props.contacts, [{ x: 1, y: 2 }]);
  assert.equal(n.props.tint, '#0f0');
  assert.deepEqual(n.props.style, { width: 10 });
  // onPress stays a runtime event (pressable), not a component event.
  assert.ok(n.props.__events.includes('press'));
  assert.equal(n.props.__componentEvents, undefined);
  render(null);
  await wait();
});

test('Animated props: prop:<name> style keys, JS driver without renders, native driver', async () => {
  const Gauge = requireNativeComponent('Gauge');
  const v = new Animated.Value(0.5);
  render(h(Gauge, { nativeID: 'g', value: v, max: 10 }));
  await wait();
  assert.equal(byId('g').props.style['prop:value'], 0.5);
  assert.equal(byId('g').props.max, 10);
  assert.equal(byId('g').props.value, undefined);
  mock.reset();
  v.setValue(0.75);
  await wait(20);
  assert.equal(byId('g').props.style['prop:value'], 0.75);
  assert.equal(mock.count('setProps'), 0);
  animated.reset();
  Animated.timing(v, { toValue: 1, duration: 100, useNativeDriver: true }).start();
  const style = animated.of('createNode').find(([, c]) => c.type === 'style');
  assert.ok(style, 'a native style node');
  assert.deepEqual(Object.keys(style[1].style), ['prop:value']);
  assert.deepEqual(animated.of('connectToView').map(([, view]) => view), [byId('g').id]);
  render(null);
  await wait();
});

test('custom events: listed for native, delivered to on<Name> with nativeEvent = data', async () => {
  const Map2 = requireNativeComponent('Minimap');
  const got = [];
  render(h(Map2, { nativeID: 'm', onMarkerPress: (e) => got.push(['marker', e.nativeEvent]), onZoom: (e) => got.push(['zoom', e.nativeEvent]) }));
  await wait();
  const n = byId('m');
  assert.deepEqual(n.props.__componentEvents, ['markerPress', 'zoom']);
  assert.ok(n.props.__events.includes('componentEvent'));
  mock.dispatch({ type: 'componentevent', target: n.id, name: 'markerPress', data: { id: 7 } });
  mock.dispatch({ type: 'componentevent', target: n.id, name: 'zoom', data: { level: 2 } });
  mock.dispatch({ type: 'componentevent', target: n.id, name: 'unknown', data: {} });
  await wait();
  assert.deepEqual(got, [
    ['marker', { id: 7 }],
    ['zoom', { level: 2 }],
  ]);
  render(null);
  await wait();
});

test('commands: codegenNativeCommands, UIManager.dispatchViewManagerCommand; hasViewManagerConfig', async () => {
  const Map3 = requireNativeComponent('Minimap');
  const Commands = codegenNativeCommands({ supportedCommands: ['zoomTo', 'ping'] });
  assert.deepEqual(Object.keys(Commands), ['zoomTo', 'ping']);
  const ref = createRef();
  render(h(Map3, { nativeID: 'c', ref }));
  await wait();
  const id = byId('c').id;
  mock.components.add('Minimap');
  mock.reset();
  assert.equal(Commands.zoomTo(ref.current, 2, { animated: true }), true);
  UIManager.dispatchViewManagerCommand(id, 'ping', [1]);
  codegenNativeCommands().anything(ref, 'x');
  assert.deepEqual(
    mock.calls.filter((c) => c[0] === 'componentCommand').map((c) => c.slice(1)),
    [
      [id, 'zoomTo', [2, { animated: true }]],
      [id, 'ping', [1]],
      [id, 'anything', ['x']],
    ],
  );
  assert.equal(hasNativeComponent('Minimap'), true);
  assert.equal(hasNativeComponent('Nope'), false);
  assert.equal(UIManager.hasViewManagerConfig('Minimap'), true);
  assert.equal(UIManager.getViewManagerConfig('Nope'), null);
  mock.components.clear();
  render(null);
  await wait();
});
