// TextInput against a bridge with native editing (the JSUI runtime's model):
// controlled / uncontrolled value reconciliation, selection, events, refs.
import { mock } from './mock-native-editing.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, useState, createRef } from 'react';
import { TextInput, Clipboard, render } from '../src/index.js';

const byNativeID = (id) => mock.find((n) => n.props.nativeID === id);
const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));

test('controlled value: accept, transform and reject native edits', async () => {
  const log = [];
  let mode = 'accept';
  function App() {
    const [v, setV] = useState('ab');
    return h(TextInput, {
      nativeID: 'ti',
      value: v,
      onChange: (e) => log.push(['change', e.nativeEvent.text, e.nativeEvent.eventCount, typeof e.nativeEvent.target]),
      onChangeText: (t) => {
        log.push(['text', t]);
        if (mode === 'accept') setV(t);
        else if (mode === 'upper') setV(t.toUpperCase());
      },
    });
  }
  render(h(App));
  await wait();
  const ti = byNativeID('ti');
  assert.equal(ti.props.value, 'ab');
  assert.equal(ti.props.mostRecentEventCount, 0);
  assert.deepEqual(ti.props.__events.includes('change') && ti.props.__events.includes('selectionChange'), true);
  assert.equal(mock.native(ti.id).text, 'ab');

  mock.edit(ti.id, 'abc');
  await wait();
  assert.equal(ti.props.value, 'abc');
  assert.equal(ti.props.mostRecentEventCount, 1);
  assert.deepEqual(log, [['change', 'abc', 1, 'number'], ['text', 'abc']]);

  mode = 'upper';
  mock.edit(ti.id, 'abcd');
  await wait();
  assert.equal(ti.props.value, 'ABCD');
  assert.equal(mock.native(ti.id).text, 'ABCD', 'native adopts the transformed value');

  mode = 'reject';
  mock.edit(ti.id, 'ABCDx');
  await wait();
  assert.equal(ti.props.value, 'ABCD');
  assert.equal(ti.props.mostRecentEventCount, 3);
  assert.equal(mock.native(ti.id).text, 'ABCD', 'rejected edit is reverted natively');
  render(null);
});

test('uncontrolled: defaultValue, edits, clear(), ref methods', async () => {
  const ref = createRef();
  const texts = [];
  render(h(TextInput, { nativeID: 'u', ref, defaultValue: 'start', onChangeText: (t) => texts.push(t) }));
  await wait();
  const ti = byNativeID('u');
  assert.equal(ti.props.value, 'start');
  mock.edit(ti.id, 'started');
  await wait();
  assert.equal(ti.props.value, 'started');
  assert.equal(mock.native(ti.id).text, 'started');
  ref.current.focus();
  assert.equal(mock.focused, ti.id);
  mock.dispatch({ type: 'focus', target: ti.id });
  assert.equal(ref.current.isFocused(), true);
  ref.current.setSelection(2, 4);
  assert.deepEqual(mock.calls.at(-1), ['setTextInputSelection', ti.id, 2, 4]);
  ref.current.setSelection(3);
  assert.deepEqual(mock.native(ti.id).selection, { start: 3, end: 3 });
  ref.current.clear();
  await wait();
  assert.equal(ti.props.value, '');
  assert.equal(mock.native(ti.id).text, '');
  assert.deepEqual(texts, ['started', '']);
  ref.current.blur();
  mock.dispatch({ type: 'blur', target: ti.id });
  assert.equal(ref.current.isFocused(), false);
  render(null);
});

test('onSelectionChange and the controlled selection prop', async () => {
  const sels = [];
  let setSel;
  function App() {
    const [sel, s] = useState({ start: 1, end: 2 });
    setSel = s;
    return h(TextInput, {
      nativeID: 's',
      defaultValue: 'hello',
      selection: sel,
      onSelectionChange: (e) => sels.push(e.nativeEvent.selection),
    });
  }
  render(h(App));
  await wait();
  const ti = byNativeID('s');
  assert.equal(ti.props.selection, undefined, 'selection is applied by command, not as a prop');
  assert.deepEqual(mock.native(ti.id).selection, { start: 1, end: 2 });
  // The user moves the caret; the owner keeps its selection: reverted.
  mock.select(ti.id, { start: 4, end: 4 });
  await wait();
  assert.deepEqual(sels, [{ start: 4, end: 4 }]);
  assert.deepEqual(mock.native(ti.id).selection, { start: 1, end: 2 });
  setSel({ start: 0, end: 5 });
  await wait();
  assert.deepEqual(mock.native(ti.id).selection, { start: 0, end: 5 });
  render(null);
});

test('Enter: submit, blurOnSubmit, submitBehavior, multiline newline', async () => {
  const log = [];
  const ref = createRef();
  function App({ extra }) {
    return h(TextInput, Object.assign({ nativeID: 'e', ref, defaultValue: 'x', onSubmitEditing: (e) => log.push('submit:' + e.nativeEvent.text) }, extra));
  }
  const enter = () => mock.dispatch({ type: 'keydown', target: byNativeID('e').id, key: 'Enter' });
  render(h(App, { extra: {} }));
  await wait();
  ref.current.focus();
  assert.equal(enter() & 2, 2, 'single line: prevented (no newline)');
  assert.deepEqual(log, ['submit:x']);
  assert.equal(mock.focused, null, 'blurred');

  render(h(App, { extra: { submitBehavior: 'submit' } }));
  await wait();
  ref.current.focus();
  enter();
  assert.equal(mock.focused, byNativeID('e').id, 'submit keeps focus');

  render(h(App, { extra: { multiline: true } }));
  await wait();
  log.length = 0;
  assert.equal(enter() & 2, 0, 'multiline: native inserts the newline');
  assert.deepEqual(log, []);

  render(h(App, { extra: { multiline: true, blurOnSubmit: true } }));
  await wait();
  ref.current.focus();
  assert.equal(enter() & 2, 2);
  assert.deepEqual(log, ['submit:x']);
  assert.equal(mock.focused, null);
  render(null);
});

test('onKeyPress, host props and Clipboard', async () => {
  const keys = [];
  render(
    h(TextInput, {
      nativeID: 'k',
      placeholder: 'Name',
      maxLength: 4,
      secureTextEntry: true,
      editable: false,
      caretHidden: true,
      selectionColor: 'red',
      cursorColor: 'blue',
      onKeyPress: (e) => {
        keys.push(e.nativeEvent.key);
        if (e.nativeEvent.key === 'q') e.preventDefault();
      },
    }),
  );
  await wait();
  const ti = byNativeID('k');
  for (const k of ['placeholder', 'maxLength', 'secureTextEntry', 'caretHidden', 'selectionColor', 'cursorColor']) assert.ok(k in ti.props, k);
  assert.equal(ti.props.editable, false);
  assert.equal(ti.props.focusable, false);
  assert.equal(mock.dispatch({ type: 'keydown', target: ti.id, key: 'a' }) & 2, 0);
  assert.equal(mock.dispatch({ type: 'keydown', target: ti.id, key: 'q' }) & 2, 2, 'onKeyPress can prevent the native edit');
  assert.deepEqual(keys, ['a', 'q']);
  Clipboard.setString('copied');
  assert.equal(globalThis.__jsui_native.getClipboard(), 'copied');
  assert.equal(await Clipboard.getString(), 'copied');
  render(null);
});
