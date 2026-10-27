// JSUIContainer against the mock bridge: what reaches native (props, inline
// source as a `load` command), the ref's postMessage / call / setState /
// patchState / reload commands, and the child's messages, calls and state
// changes arriving as containerevent events. The child runtime itself is
// native: tests/native/container_test.cpp.
import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, createRef } from 'react';
import { JSUIContainer, render } from '../src/index.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const byId = (id) => mock.find((x) => x.props.nativeID === id);
const commands = () => mock.calls.filter((c) => c[0] === 'containerCommand').map((c) => c[2]);

test('props reach native; bundleData goes once as a load command', async () => {
  mock.reset();
  const src = 'globalThis.x = 1;';
  render(
    h(JSUIContainer, {
      nativeID: 'c',
      bundleData: src,
      appKey: 'Inner',
      initialProps: { seed: 7 },
      hostState: { theme: 'dark' },
      onMessage: () => {},
      style: { width: 200, height: 100 },
    }),
  );
  await wait();
  const n = byId('c');
  assert.equal(n.type, 'JSUIContainer');
  assert.equal(n.props.appKey, 'Inner');
  assert.deepEqual(n.props.initialProps, { seed: 7 });
  assert.deepEqual(n.props.hostState, { theme: 'dark' });
  assert.equal(n.props.bundleData, undefined);
  assert.equal(n.props.onMessage, undefined);
  assert.deepEqual(commands(), [{ op: 'load', data: src }]);
  // Same source again: no new load.
  render(h(JSUIContainer, { nativeID: 'c', bundleData: src, appKey: 'Inner', style: { width: 200, height: 100 } }));
  await wait();
  assert.equal(commands().length, 1);
  render(h(JSUIContainer, { nativeID: 'c', bundleSrc: 'apps/b.js', style: { width: 200, height: 100 } }));
  await wait();
  assert.equal(byId('c').props.bundleSrc, 'apps/b.js');
  render(null);
  await wait();
});

test('ref commands and the child talking back', async () => {
  mock.reset();
  const ref = createRef();
  const got = [];
  render(
    h(JSUIContainer, {
      nativeID: 'c',
      ref,
      bundleSrc: 'apps/a.js',
      onMessage: (name, payload) => got.push(['message', name, payload]),
      onStateChange: (key, value) => got.push(['state', key, value]),
      onError: (msg) => got.push(['error', msg]),
      onCall: async (name, args) => {
        if (name === 'fail') throw new Error('nope');
        return { echo: args, name };
      },
    }),
  );
  await wait();
  const id = byId('c').id;
  const c = ref.current;
  assert.equal(c.id, id);

  assert.equal(c.postMessage('hello', { n: 1 }), true);
  c.setState('gold', 120);
  c.setState('gone', undefined);
  c.patchState('inv', { a: 1 });
  c.reload();
  assert.deepEqual(commands(), [
    { op: 'post', name: 'hello', payload: { n: 1 } },
    { op: 'state', key: 'gold', value: 120 },
    { op: 'state', key: 'gone', remove: true },
    { op: 'state', key: 'inv', value: { a: 1 }, patch: true },
    { op: 'reload' },
  ]);

  // Parent -> child call, answered by a callResult event.
  mock.reset();
  const p = c.call('ui.confirm', { text: 'Quit?' });
  const [cmd] = commands();
  assert.equal(cmd.op, 'call');
  assert.equal(cmd.name, 'ui.confirm');
  assert.deepEqual(cmd.args, { text: 'Quit?' });
  mock.dispatch({ type: 'containerevent', target: id, kind: 'callResult', seq: cmd.seq, ok: true, result: { ok: 1 } });
  assert.deepEqual(await p, { ok: 1 });
  const q = assert.rejects(c.call('missing'), /no handler/);
  mock.dispatch({ type: 'containerevent', target: id, kind: 'callResult', seq: commands()[1].seq, ok: false, error: 'no handler' });
  await q;

  // Child -> parent: messages, state, errors, calls.
  mock.reset();
  mock.dispatch({ type: 'containerevent', target: id, kind: 'message', name: 'score', payload: { v: 3 } });
  mock.dispatch({ type: 'containerevent', target: id, kind: 'state', key: 'volume', value: 0.5 });
  mock.dispatch({ type: 'containerevent', target: id, kind: 'state', key: 'volume', value: null, removed: true });
  mock.dispatch({ type: 'containerevent', target: id, kind: 'error', message: 'boom' });
  assert.deepEqual(got, [
    ['message', 'score', { v: 3 }],
    ['state', 'volume', 0.5],
    ['state', 'volume', undefined],
    ['error', 'boom'],
  ]);
  mock.dispatch({ type: 'containerevent', target: id, kind: 'call', callId: 5, name: 'shop.buy', args: { item: 'potion' } });
  mock.dispatch({ type: 'containerevent', target: id, kind: 'call', callId: 6, name: 'fail', args: null });
  await wait();
  assert.deepEqual(commands(), [
    { op: 'resolve', callId: 5, value: { echo: { item: 'potion' }, name: 'shop.buy' } },
    { op: 'reject', callId: 6, message: 'nope' },
  ]);

  // Unmounting fails the calls still waiting.
  const late = assert.rejects(c.call('slow'), /unmounted/);
  render(null);
  await wait();
  await late;
});

test('calls without an onCall handler are rejected', async () => {
  mock.reset();
  render(h(JSUIContainer, { nativeID: 'c', bundleSrc: 'a.js' }));
  await wait();
  const id = byId('c').id;
  mock.dispatch({ type: 'containerevent', target: id, kind: 'call', callId: 9, name: 'x', args: null });
  assert.deepEqual(commands(), [{ op: 'reject', callId: 9, message: 'no onCall handler for x' }]);
  render(null);
  await wait();
});
