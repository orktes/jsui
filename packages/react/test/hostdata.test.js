// Game data: shared host state (HostState / useHostState) and calls
// (Host.call / Host.handle) against the mock bridge.
import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, memo } from 'react';
import {
  View,
  Text,
  render,
  Host,
  HostState,
  useHostState,
  useHostStateSelector,
  useHostHandler,
} from '../src/index.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const textOf = (id) => {
  const n = mock.find((x) => x.props.nativeID === id);
  if (!n) return null;
  const out = [];
  const rec = (i) => {
    const x = mock.nodes.get(i);
    if (x.type === 'RawText') out.push(x.text);
    else x.children.forEach(rec);
  };
  rec(n.id);
  return out.join('');
};

test('useHostState: fallback, state set before mount, per-key re-renders', async () => {
  mock.setHostState('gold', '120');
  mock.tickState();
  const renders = { gold: 0, gems: 0 };
  function Gold() {
    renders.gold++;
    return h(Text, { nativeID: 'gold' }, String(useHostState('gold', 0)));
  }
  function Gems() {
    renders.gems++;
    return h(Text, { nativeID: 'gems' }, String(useHostState('gems', 7)));
  }
  render(h(View, null, h(Gold), h(Gems)));
  await wait();
  assert.equal(textOf('gold'), '120'); // set before mount: seen immediately
  assert.equal(textOf('gems'), '7'); // unset: fallback
  const before = { ...renders };

  mock.setHostState('gold', '150');
  mock.tickState();
  await wait();
  assert.equal(textOf('gold'), '150');
  assert.equal(renders.gold, before.gold + 1);
  assert.equal(renders.gems, before.gems, 'only subscribers of the changed key re-render');

  mock.setHostState('gems', '3');
  mock.setHostState('gold', undefined); // removed -> fallback
  mock.tickState();
  await wait();
  assert.equal(textOf('gems'), '3');
  assert.equal(textOf('gold'), '0');
  render(null);
});

test('HostState.get parses once per change; late readers see the current value', () => {
  mock.setHostState('inv', '{"items":{"a":{"n":1}}}');
  const a = HostState.get('inv');
  assert.deepEqual(a, { items: { a: { n: 1 } } });
  assert.equal(HostState.get('inv'), a, 'cached by version');
  // Not delivered yet (no tick), but reads see the latest value.
  mock.setHostState('inv', '{"items":{"a":{"n":2}}}');
  assert.equal(HostState.get('inv').items.a.n, 2);
  mock.tickState();
  assert.equal(HostState.get('nope'), undefined);
  assert.ok(HostState.keys().includes('inv'));
  assert.ok(Object.isFrozen(HostState.get('inv').items), 'development builds freeze shared values');
});

test('structural sharing keeps unchanged parts identical', async () => {
  mock.setHostState('bag', JSON.stringify({ gold: 1, items: { sword: { lvl: 3 }, shield: { lvl: 1 } }, tags: ['a', 'b'] }));
  const v1 = HostState.get('bag');
  mock.setHostState('bag', JSON.stringify({ gold: 2, items: { sword: { lvl: 3 }, shield: { lvl: 2 } }, tags: ['a', 'b'] }));
  const v2 = HostState.get('bag');
  assert.notEqual(v1, v2);
  assert.equal(v2.items.sword, v1.items.sword);
  assert.notEqual(v2.items.shield, v1.items.shield);
  assert.equal(v2.tags, v1.tags);

  // React.memo rows skip re-rendering for unchanged items.
  const rows = [];
  const Row = memo(({ id, item }) => {
    rows.push(id);
    return h(Text, null, id + ':' + item.lvl);
  });
  function Bag() {
    const bag = useHostState('bag', { items: {} });
    return h(View, null, Object.entries(bag.items).map(([id, item]) => h(Row, { key: id, id, item })));
  }
  render(h(Bag));
  await wait();
  rows.length = 0;
  mock.setHostState('bag', JSON.stringify({ gold: 3, items: { sword: { lvl: 3 }, shield: { lvl: 5 } }, tags: ['a', 'b'] }));
  mock.tickState();
  await wait();
  assert.deepEqual(rows, ['shield']);
  render(null);
});

test('useHostStateSelector re-renders only when the selection changes', async () => {
  mock.setHostState('player', '{"gold":10,"xp":1}');
  let renders = 0;
  function Gold() {
    renders++;
    return h(Text, { nativeID: 'pgold' }, String(useHostStateSelector('player', (p) => p.gold, { gold: 0 })));
  }
  render(h(Gold));
  await wait();
  assert.equal(textOf('pgold'), '10');
  const r = renders;
  mock.setHostState('player', '{"gold":10,"xp":2}');
  mock.tickState();
  await wait();
  assert.equal(renders, r, 'xp changed, gold did not');
  mock.setHostState('player', '{"gold":11,"xp":2}');
  mock.tickState();
  await wait();
  assert.equal(textOf('pgold'), '11');
  render(null);
});

test('HostState.set / patch: UI-owned keys notify JS subscribers and the host', async () => {
  const seen = [];
  const off = HostState.subscribe('settings', (v, k) => seen.push([k, v]));
  mock.uiState.length = 0;
  HostState.set('settings', { volume: 0.5, lang: 'en' });
  assert.deepEqual(seen.at(-1), ['settings', { volume: 0.5, lang: 'en' }]);
  HostState.patch('settings', { volume: 0.8, lang: null });
  assert.deepEqual(HostState.get('settings'), { volume: 0.8 });
  assert.equal(seen.length, 2);
  HostState.set('settings', { volume: 0.8 }); // unchanged: no notification
  assert.equal(seen.length, 2);
  assert.deepEqual(mock.uiState.map(([k]) => k), ['settings', 'settings']);
  HostState.set('settings', undefined);
  assert.equal(HostState.get('settings'), undefined);
  assert.deepEqual(seen.at(-1), ['settings', undefined]);
  assert.throws(() => HostState.set('bad', () => 1), TypeError);
  off();
  HostState.set('settings', { volume: 1 });
  assert.equal(seen.length, 3);
});

test('wildcard subscription and listener errors do not break others', () => {
  const seen = [];
  const off = HostState.subscribe('*', (_v, k) => seen.push(k));
  const errors = [];
  const prevReport = globalThis.__jsui_reportError;
  globalThis.__jsui_reportError = (e) => errors.push(e.message);
  const offBad = HostState.subscribe('w1', () => {
    throw new Error('boom');
  });
  mock.setHostState('w1', '1');
  mock.setHostState('w2', '2');
  mock.tickState();
  assert.deepEqual(seen, ['w1', 'w2']);
  assert.deepEqual(errors, ['boom']);
  globalThis.__jsui_reportError = prevReport;
  off();
  offBad();
});

test('Host.call resolves, rejects, times out and aborts', async () => {
  mock.onCall = null;
  const p1 = Host.call('shop.buy', { item: 'potion' });
  const [id1, c1] = Array.from(mock.hostCalls).at(-1);
  assert.equal(c1.name, 'shop.buy');
  assert.equal(c1.json, '{"item":"potion"}');
  mock.resolveCall(id1, '{"ok":true,"gold":90}');
  assert.deepEqual(await p1, { ok: true, gold: 90 });

  const p2 = Host.call('shop.buy', { item: 'castle' });
  const [id2] = Array.from(mock.hostCalls).at(-1);
  mock.rejectCall(id2, 'not enough gold');
  await assert.rejects(p2, (e) => e.name === 'HostCallError' && e.message === 'not enough gold' && e.call === 'shop.buy');

  // Answered synchronously from inside on_call.
  mock.onCall = (id) => mock.resolveCall(id, '42');
  assert.equal(await Host.call('sync'), 42);
  mock.onCall = null;

  mock.cancelled.length = 0;
  const p3 = Host.call('slow', null, { timeout: 20 });
  const [id3] = Array.from(mock.hostCalls).at(-1);
  await assert.rejects(p3, (e) => e.code === 'timeout');
  assert.deepEqual(mock.cancelled, [id3]);
  assert.equal(mock.resolveCall(id3, '1'), -1, 'cancelled call is gone');

  const ac = new AbortController();
  const p4 = Host.call('slow', null, { signal: ac.signal });
  ac.abort(new Error('closed'));
  await assert.rejects(p4, /closed/);

  mock.callsEnabled = false;
  await assert.rejects(Host.call('x'), /on_call is not set/);
  mock.callsEnabled = true;
  await assert.rejects(Host.call('x', { f: 1n }), TypeError);
});

test('Host.handle / useHostHandler answer jsui_call_ui', async () => {
  mock.uiResults.length = 0;
  const off = Host.handle('double', ({ n }) => n * 2);
  globalThis.__jsui_onCall(1, 'double', '{"n":21}');
  await wait();
  assert.deepEqual(mock.uiResults.at(-1), [1, true, '42']);

  Host.handle('async', async () => {
    await wait(5);
    return { done: true };
  });
  globalThis.__jsui_onCall(2, 'async', 'null');
  await wait(20);
  assert.deepEqual(mock.uiResults.at(-1), [2, true, '{"done":true}']);

  Host.handle('throws', () => {
    throw new Error('nope');
  });
  globalThis.__jsui_onCall(3, 'throws', 'null');
  assert.deepEqual(mock.uiResults.at(-1), [3, false, 'nope']);

  globalThis.__jsui_onCall(4, 'missing', 'null');
  assert.equal(mock.uiResults.at(-1)[1], false);
  assert.match(mock.uiResults.at(-1)[2], /no UI handler for "missing"/);

  off();
  globalThis.__jsui_onCall(5, 'double', '{"n":1}');
  assert.equal(mock.uiResults.at(-1)[1], false);

  // A component-scoped handler that updates React state.
  function Dialog() {
    const [text, setText] = [useHostState('dlg', ''), (t) => HostState.set('dlg', t)];
    useHostHandler('ui.confirm', ({ text: t }) => {
      setText(t);
      return 'yes';
    });
    return h(Text, { nativeID: 'dlg' }, text);
  }
  render(h(Dialog));
  await wait();
  globalThis.__jsui_onCall(6, 'ui.confirm', '{"text":"Quit?"}');
  await wait();
  assert.deepEqual(mock.uiResults.at(-1), [6, true, '"yes"']);
  assert.equal(textOf('dlg'), 'Quit?');
  render(null);
  await wait();
  globalThis.__jsui_onCall(7, 'ui.confirm', '{}');
  assert.equal(mock.uiResults.at(-1)[1], false, 'unregistered on unmount');
});
