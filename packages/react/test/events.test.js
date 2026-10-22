import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, useState } from 'react';
import {
  View,
  Text,
  Pressable,
  TouchableOpacity,
  TextInput,
  Modal,
  PanResponder,
  render,
  Host,
  useHostMessage,
  useGamepad,
  Dimensions,
  useWindowDimensions,
  Button,
  Switch,
} from '../src/index.js';

const byNativeID = (id) => mock.find((n) => n.props.nativeID === id);
const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));

test('capture then bubble with stopPropagation and synthetic event shape', () => {
  const log = [];
  render(
    h(
      View,
      {
        nativeID: 'outer',
        onPointerDownCapture: (e) => log.push('outer-capture:' + e.currentTarget),
        onPointerDown: (e) => log.push('outer-bubble'),
      },
      h(View, {
        nativeID: 'inner',
        onPointerDown: (e) => {
          log.push('inner:' + e.nativeEvent.locationX + ',' + e.nativeEvent.pageY + ':' + (e.target === e.currentTarget));
        },
      }),
    ),
  );
  const outer = byNativeID('outer');
  const inner = byNativeID('inner');
  assert.deepEqual(inner.props.__events, ['pointerDown'], 'plain pointer listeners are not "press" (not focusable)');
  assert.ok(outer.props.__events.includes('pointerDown'));
  const handled = mock.dispatch({ type: 'pointerdown', target: inner.id, x: 50, y: 60, localX: 5, localY: 6, button: 0, pointerType: 'mouse' });
  assert.equal(handled, 1, 'handler ran, no preventDefault');
  assert.deepEqual(log, ['outer-capture:' + outer.id, 'inner:5,60:true', 'outer-bubble']);
  assert.equal(mock.dispatch({ type: 'pointerup', target: inner.id, x: 0, y: 0 }), 0, 'no handler -> 0');
  render(null);
});

test('Pressable: press, long press, cancel on leave, function style', async () => {
  const log = [];
  render(
    h(
      Pressable,
      {
        nativeID: 'p',
        onPress: () => log.push('press'),
        onPressIn: () => log.push('in'),
        onPressOut: () => log.push('out'),
        onLongPress: () => log.push('long'),
        delayLongPress: 30,
        style: ({ pressed }) => ({ backgroundColor: pressed ? 'red' : 'blue' }),
      },
      ({ pressed }) => h(Text, null, pressed ? 'down' : 'up'),
    ),
  );
  const p = () => byNativeID('p');
  assert.equal(p().props.focusable, true);
  assert.ok(p().props.__events.includes('click'));
  assert.ok(p().props.__events.includes('hoverIn'));
  assert.ok(p().props.__events.includes('focus'));
  const ev = (type, extra) => mock.dispatch(Object.assign({ type, target: p().id, x: 1, y: 1, button: 0 }, extra));
  ev('pointerdown');
  await wait();
  assert.equal(p().props.style.backgroundColor, 'red');
  assert.equal(mock.text(p().id), 'down');
  ev('pointerup');
  ev('click');  // native click after pointerup must not double-fire
  await wait();
  assert.deepEqual(log, ['in', 'out', 'press']);
  assert.equal(p().props.style.backgroundColor, 'blue');

  log.length = 0;
  ev('pointerdown');
  await wait(60);
  ev('pointerup');
  assert.deepEqual(log, ['in', 'long', 'out'], 'long press suppresses onPress');

  log.length = 0;
  ev('pointerdown');
  ev('pointerleave');
  ev('pointerup');
  assert.deepEqual(log, ['in', 'out'], 'leaving cancels');
  render(null);
});

test('Pressable: keyboard Enter and gamepad click activate; disabled ignores', async () => {
  const log = [];
  let setDisabled;
  function App() {
    const [disabled, sd] = useState(false);
    setDisabled = sd;
    return h(Pressable, { nativeID: 'k', disabled, onPress: (e) => log.push('press:' + e.type), onFocus: () => log.push('focus') });
  }
  render(h(App));
  const k = () => byNativeID('k');
  mock.dispatch({ type: 'focus', target: k().id });
  // Native would synthesize click for Enter unless the keydown is handled.
  assert.equal(mock.dispatch({ type: 'keydown', target: k().id, key: 'Enter', code: 'Enter' }), 3, 'handled + preventDefault so native does not click');
  mock.dispatch({ type: 'keyup', target: k().id, key: 'Enter', code: 'Enter' });
  mock.advance(450);
  mock.dispatch({ type: 'click', target: k().id });  // gamepad A
  assert.deepEqual(log, ['focus', 'press:keyup', 'press:click']);
  setDisabled(true);
  await wait();
  assert.equal(k().props.focusable, false);
  log.length = 0;
  mock.advance(450);
  mock.dispatch({ type: 'click', target: k().id });
  mock.dispatch({ type: 'pointerdown', target: k().id, x: 0, y: 0, button: 0 });
  mock.dispatch({ type: 'pointerup', target: k().id, x: 0, y: 0, button: 0 });
  assert.deepEqual(log, []);
  render(null);
});

test('nested pressables: only the innermost fires', async () => {
  const log = [];
  render(h(Pressable, { nativeID: 'o', onPress: () => log.push('outer') }, h(Pressable, { nativeID: 'i', onPress: () => log.push('inner') })));
  const i = byNativeID('i');
  mock.dispatch({ type: 'pointerdown', target: i.id, x: 0, y: 0, button: 0 });
  mock.dispatch({ type: 'pointerup', target: i.id, x: 0, y: 0, button: 0 });
  assert.deepEqual(log, ['inner']);
  render(null);
});

test('hover in/out (non-bubbling enter/leave)', () => {
  const log = [];
  render(h(Pressable, { nativeID: 'h', onHoverIn: () => log.push('in'), onHoverOut: () => log.push('out') }, h(View, { nativeID: 'child' })));
  mock.dispatch({ type: 'pointerenter', target: byNativeID('child').id, x: 0, y: 0 });
  assert.deepEqual(log, [], 'enter on child does not bubble to parent');
  mock.dispatch({ type: 'pointerenter', target: byNativeID('h').id, x: 0, y: 0 });
  mock.dispatch({ type: 'pointerleave', target: byNativeID('h').id, x: 0, y: 0 });
  assert.deepEqual(log, ['in', 'out']);
  render(null);
});

test('TouchableOpacity animates opacity via patchStyle without re-rendering', () => {
  let renders = 0;
  function Child() {
    renders++;
    return h(Text, null, 'x');
  }
  render(h(TouchableOpacity, { nativeID: 't', onPress() {}, style: { opacity: 1 } }, h(Child)));
  const t = byNativeID('t');
  mock.reset();
  mock.dispatch({ type: 'pointerdown', target: t.id, x: 0, y: 0, button: 0, pointerType: 'mouse' });
  mock.advance(200);
  assert.ok(Math.abs(t.props.style.opacity - 0.2) < 1e-6, 'opacity reached activeOpacity: ' + t.props.style.opacity);
  assert.equal(mock.count('setProps'), 0);
  assert.ok(mock.count('patchStyle') > 3);
  mock.dispatch({ type: 'pointerup', target: t.id, x: 0, y: 0, button: 0 });
  mock.advance(300);
  assert.equal(t.props.style.opacity, 1);
  assert.equal(renders, 1, 'children never re-rendered');
  render(null);
});

test('Button renders like react-native-web and presses', () => {
  let n = 0;
  render(h(Button, { title: 'Play', onPress: () => n++, color: '#f00' }));
  const btn = mock.nodes.get(mock.root().children[0]);
  assert.equal(btn.props.style.backgroundColor, '#f00');
  assert.equal(mock.text(btn.id), 'Play');
  mock.dispatch({ type: 'click', target: btn.id });
  assert.equal(n, 1);
  render(null);
});

test('TextInput: typing, backspace (code points), submit, controlled value', async () => {
  const log = [];
  function App() {
    const [v, setV] = useState('');
    return h(TextInput, {
      nativeID: 'ti',
      value: v,
      placeholder: 'Name',
      onChangeText: (t) => {
        log.push(t);
        setV(t);
      },
      onSubmitEditing: (e) => log.push('submit:' + e.nativeEvent.text),
    });
  }
  render(h(App));
  const ti = () => byNativeID('ti');
  assert.equal(ti().type, 'TextInput');
  assert.equal(ti().props.placeholder, 'Name');
  mock.dispatch({ type: 'pointerdown', target: ti().id, x: 0, y: 0, button: 0 });
  assert.equal(mock.focused, ti().id);
  mock.dispatch({ type: 'textinput', target: ti().id, text: 'hé' });
  mock.dispatch({ type: 'textinput', target: ti().id, text: '😀' });
  await wait();
  assert.equal(ti().props.value, 'hé😀');
  mock.dispatch({ type: 'keydown', target: ti().id, key: 'Backspace' });
  await wait();
  assert.equal(ti().props.value, 'hé');
  mock.dispatch({ type: 'keydown', target: ti().id, key: 'Enter' });
  assert.deepEqual(log, ['hé', 'hé😀', 'hé', 'submit:hé']);
  assert.equal(mock.focused, null, 'blurOnSubmit');
  render(null);
});

test('Switch toggles', async () => {
  function App() {
    const [on, setOn] = useState(false);
    return h(Switch, { nativeID: 'sw', value: on, onValueChange: setOn });
  }
  render(h(App));
  const sw = () => byNativeID('sw');
  const thumb = () => mock.nodes.get(sw().children[1]);
  assert.equal(thumb().props.style.marginLeft, 0);
  mock.dispatch({ type: 'click', target: sw().id });
  await wait();
  assert.equal(thumb().props.style.marginLeft, 20);
  render(null);
});

test('Modal renders in a root portal and closes on Escape / gamepad B', async () => {
  const log = [];
  function App() {
    const [open, setOpen] = useState(true);
    return h(
      View,
      { nativeID: 'app', style: { padding: 10 } },
      h(Modal, { transparent: true, visible: open, onRequestClose: () => (log.push('close'), setOpen(false)), onShow: () => log.push('show') }, h(Text, { nativeID: 'mtext' }, 'menu')),
    );
  }
  render(h(App));
  const app = byNativeID('app');
  assert.equal(app.children.length, 0, 'modal content is not inside the parent');
  const text = byNativeID('mtext');
  const host = mock.nodes.get(text.parent);
  assert.equal(host.parent, 1, 'modal host is a root child');
  assert.equal(host.props.style.position, 'absolute');
  mock.dispatch({ type: 'keydown', target: text.id, key: 'Escape' });
  await wait();
  assert.deepEqual(log, ['show', 'close']);
  assert.equal(byNativeID('mtext'), null);
  render(null);
});

test('PanResponder drag gesture', () => {
  const moves = [];
  let released = null;
  const pan = PanResponder.create({
    onStartShouldSetPanResponder: () => true,
    onPanResponderMove: (e, gs) => moves.push([gs.dx, gs.dy]),
    onPanResponderRelease: (e, gs) => (released = [gs.dx, gs.dy]),
  });
  render(h(View, Object.assign({ nativeID: 'drag' }, pan.panHandlers), h(View, { nativeID: 'knob' })));
  const knob = byNativeID('knob');
  const drag = byNativeID('drag');
  assert.ok(drag.props.__events.includes('responder'));
  assert.ok(!drag.props.__events.includes('press'));
  mock.dispatch({ type: 'pointerdown', target: knob.id, x: 10, y: 10, buttons: 1, timestamp: 0 });
  mock.dispatch({ type: 'pointermove', target: knob.id, x: 15, y: 12, buttons: 1, timestamp: 16 });
  // Pointer captured but now over nothing we know: still delivered to the responder.
  mock.dispatch({ type: 'pointermove', target: 9999, x: 30, y: 20, buttons: 1, timestamp: 32 });
  mock.dispatch({ type: 'pointerup', target: 9999, x: 31, y: 21, timestamp: 48 });
  assert.deepEqual(moves, [
    [5, 2],
    [20, 10],
  ]);
  assert.deepEqual(released, [21, 11]);
  render(null);
});

test('onLayout, host messages, postMessage', async () => {
  const layouts = [];
  const got = [];
  function App() {
    useHostMessage('score', (d) => got.push(d));
    return h(View, { nativeID: 'l', onLayout: (e) => layouts.push(e.nativeEvent.layout) });
  }
  render(h(App));
  await wait();
  globalThis.__jsui_onLayout(byNativeID('l').id, { x: 1, y: 2, width: 3, height: 4 });
  assert.deepEqual(layouts, [{ x: 1, y: 2, width: 3, height: 4 }]);
  globalThis.__jsui_onHostMessage('score', '{"value":42}');
  assert.deepEqual(got, [{ value: 42 }]);
  Host.postMessage('quit', { now: true });
  assert.deepEqual(mock.messages.at(-1), ['quit', '{"now":true}']);
  const off = Host.onMessage('x', (d) => got.push(d));
  off();
  globalThis.__jsui_onHostMessage('x', '1');
  assert.equal(got.length, 1);
  render(null);
});

test('Dimensions + useWindowDimensions follow resize events', async () => {
  assert.deepEqual(Dimensions.get('window'), { width: 800, height: 600, scale: 2, fontScale: 1 });
  const seen = [];
  function App() {
    const { width, height } = useWindowDimensions();
    seen.push(width + 'x' + height);
    return null;
  }
  render(h(App));
  mock.dispatch({ type: 'resize', width: 1024, height: 768, scale: 1 });
  await wait();
  assert.deepEqual(seen, ['800x600', '1024x768']);
  assert.equal(Dimensions.get('screen').width, 1024);
  render(null);
});

test('useGamepad tracks buttons and lets handlers preventDefault', async () => {
  let state;
  function App() {
    state = useGamepad({ onButton: (e) => e.buttonIndex === 9 && e.preventDefault() });
    return null;
  }
  render(h(App));
  mock.dispatch({ type: 'gamepadconnected', target: 1, gamepad: 0, connected: true, id: 'Pad' });
  const handled = mock.dispatch({ type: 'gamepadbutton', target: 1, gamepad: 0, buttonIndex: 9, value: 1, pressed: true });
  await wait();
  assert.equal(handled, 3);
  assert.equal(state.connected, true);
  assert.equal(state.buttons[9], 1);
  render(null);
});

test('handler errors are rethrown after dispatch completes', () => {
  const log = [];
  render(
    h(View, { nativeID: 'a', onClick: () => log.push('outer') }, h(View, { nativeID: 'b', onClick: () => {
      throw new Error('boom');
    } })),
  );
  assert.throws(() => mock.dispatch({ type: 'click', target: byNativeID('b').id }), /boom/);
  assert.deepEqual(log, ['outer'], 'propagation continued');
  render(null);
});

test('gamepad-synthesized click right after a mouse press still activates', () => {
  let n = 0;
  render(h(Pressable, { nativeID: 'g', onPress: () => n++ }));
  const g = byNativeID('g');
  mock.dispatch({ type: 'pointerdown', target: g.id, x: 0, y: 0, button: 0 });
  mock.dispatch({ type: 'pointerup', target: g.id, x: 0, y: 0, button: 0 });
  mock.dispatch({ type: 'click', target: g.id, pointerType: 'mouse' });
  assert.equal(n, 1);
  mock.dispatch({ type: 'click', target: g.id, pointerType: 'gamepad' });
  assert.equal(n, 2);
  render(null);
});

test('host message state updates commit in the same tick (microtask, no timer)', async () => {
  function App() {
    const [hp, setHp] = useState(100);
    useHostMessage('hp', setHp);
    return h(Text, { nativeID: 'hp' }, String(hp));
  }
  render(h(App));
  await wait();
  globalThis.__jsui_onHostMessage('hp', '42');
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(mock.text(byNativeID('hp').id), '42');
  render(null);
});
