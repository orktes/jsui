// Mock bridge with native TextInput editing (like the JSUI runtime): the
// mock keeps each TextInput's native text, adopts `value` when
// `mostRecentEventCount` has caught up, and `mock.edit(id, text, sel)`
// simulates a user edit (change + selectionchange events). Import this
// before @jsui/react.
import { mock } from './mock-native.js';

const bridge = globalThis.__jsui_native;
const edits = new Map();  // id -> {text, count, selection}
let clipboard = '';

function stateOf(id) {
  let s = edits.get(id);
  if (!s) edits.set(id, (s = { text: '', count: 0, selection: { start: 0, end: 0 } }));
  return s;
}

bridge.textEditing = true;
const setProps = bridge.setProps;
bridge.setProps = (id, props) => {
  setProps(id, props);
  const n = mock.nodes.get(id);
  if (n.type !== 'TextInput' || typeof props.value !== 'string') return;
  const s = stateOf(id);
  const count = props.mostRecentEventCount;
  if (typeof count === 'number' && count < s.count) return;  // stale render
  if (props.value !== s.text) {
    s.text = props.value;
    s.selection = { start: s.text.length, end: s.text.length };
  }
};
bridge.setTextInputSelection = (id, start, end) => {
  stateOf(id).selection = { start, end: end == null ? start : end };
  mock.calls.push(['setTextInputSelection', id, start, end]);
};
bridge.getClipboard = () => clipboard;
bridge.setClipboard = (t) => {
  clipboard = String(t);
};

mock.native = (id) => stateOf(id);
/** A user edit: native text becomes `text`, the caret goes to `sel`. */
mock.edit = (id, text, sel = { start: text.length, end: text.length }) => {
  const s = stateOf(id);
  s.text = text;
  s.count++;
  s.selection = sel;
  mock.dispatch({ type: 'change', target: id, text, eventCount: s.count });
  mock.dispatch({ type: 'selectionchange', target: id, selection: sel });
};
/** A caret move only. */
mock.select = (id, sel) => {
  stateOf(id).selection = sel;
  mock.dispatch({ type: 'selectionchange', target: id, selection: sel });
};

export { mock };
