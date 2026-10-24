import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import { HOST_FLAGS, patchScheduler, specializeReconciler } from '../src/specialize.js';
import { jsuiReactDir } from '../src/config.js';
import { build } from '../src/build.js';

const require = createRequire(import.meta.url);

test('specialize: every patch applies to the installed react-reconciler', async () => {
  const file = require.resolve('react-reconciler/cjs/react-reconciler.production.js', { paths: [jsuiReactDir()] });
  const { applied, failed } = specializeReconciler(await readFile(file, 'utf8'));
  assert.deepEqual(failed, []);
  assert.ok(applied.length > 0);
});

test('specialize: what it folds matches the host config', async () => {
  const { hostConfig } = await import('../../react/src/renderer.js');
  for (const [k, v] of Object.entries(HOST_FLAGS)) assert.equal(hostConfig[k], v, k);
  assert.equal(hostConfig.shouldSetTextContent('Text', { children: 'x' }), false);
  const inst = {};
  assert.equal(hostConfig.getPublicInstance(inst), inst);
  assert.equal(hostConfig.createFragmentInstance({}), null);
});

// A host bridge that records the tree (as in cli.test.js).
function run(code) {
  const nodes = new Map([[1, { type: 'Root', props: {}, children: [], text: '' }]]);
  let next = 2;
  const ctx = {
    console: { log() {}, warn() {}, error: (...a) => ctx.errors.push(a.join(' ')), info() {}, debug() {} },
    errors: [],
    setTimeout: () => 0,
    clearTimeout() {},
    __jsui_native: {
      rootId: 1,
      createNode(type) {
        nodes.set(next, { type, props: {}, children: [], text: '' });
        return next++;
      },
      setProps(id, p) {
        nodes.get(id).props = JSON.parse(JSON.stringify(p));
      },
      setText(id, t) {
        nodes.get(id).text = t;
      },
      appendChild(p, c) {
        nodes.get(p).children.push(c);
      },
      insertBefore(p, c, b) {
        const k = nodes.get(p).children;
        k.splice(k.indexOf(b), 0, c);
      },
      removeChild(p, c) {
        const k = nodes.get(p).children;
        k.splice(k.indexOf(c), 1);
      },
      destroyNode() {},
    },
  };
  ctx.globalThis = ctx;
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  const texts = () => {
    const out = [];
    const walk = (id) => {
      const n = nodes.get(id);
      if (n.type === 'RawText') out.push(n.text);
      for (const c of n.children) walk(c);
    };
    walk(1);
    return out.join('|');
  };
  return { ctx, texts };
}

// Context changes under 300 bailing-out memo rows, consumers deep inside a
// few of them and at the top, a nested provider of another context, and
// renders where the values stay the same: the patched reconciler must render
// exactly what the published one does.
const CONTEXT_APP = `
import React, { createContext, memo, useContext, useState } from 'react';
import { Text, View, render, flushSync } from 'react-native';
const Theme = createContext('light');
const Other = createContext(0);
const renders = (globalThis.renders = {});
const count = (k) => (renders[k] = (renders[k] || 0) + 1);
function Deep({ id }) {
  const theme = useContext(Theme);
  const other = useContext(Other);
  count('deep' + id);
  return <Text>{id + ':' + theme + ':' + other}</Text>;
}
const Wrap = ({ children }) => <View>{children}</View>;
const Row = memo(function Row({ i }) {
  count('row');
  let body = <Text>{'row' + i}</Text>;
  if (i % 97 === 5) body = <Wrap><Wrap><Wrap><Deep id={i} /></Wrap></Wrap></Wrap>;
  if (i === 150) body = <Other.Provider value={7}><Wrap><Deep id={i} /></Wrap></Other.Provider>;
  return <View>{body}</View>;
});
const rows = [];
for (let i = 0; i < 300; i++) rows.push(i);
function App() {
  const [s, set] = useState({ theme: 'light', other: 1, tick: 0 });
  globalThis.set = (v) => flushSync(() => set((o) => Object.assign({}, o, v)));
  count('app');
  return (
    <Theme.Provider value={s.theme}>
      <Other.Provider value={s.other}>
        <View>
          <Deep id="top" />
          <Text>{'tick' + s.tick}</Text>
          {rows.map((i) => <Row key={i} i={i} />)}
        </View>
      </Other.Provider>
    </Theme.Provider>
  );
}
render(<App />);
`;

test('specialize: context propagation under bailing-out memo rows renders as unpatched React', async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-spec-'));
  await writeFile(path.join(dir, 'App.js'), CONTEXT_APP);
  const results = [];
  for (const patches of ['1', '0']) {
    process.env.JSUI_REACT_PATCHES = patches;
    const out = path.join(dir, patches, 'bundle.js');
    try {
      await build({ entry: path.join(dir, 'App.js'), outfile: out });
    } finally {
      delete process.env.JSUI_REACT_PATCHES;
    }
    const code = await readFile(out, 'utf8');
    assert.equal(/\.return\.flags \|= 262144/.test(code), patches === '1');
    const { ctx, texts } = run(code);
    const log = [];
    const step = (v) => {
      if (v) ctx.set(v);
      log.push(texts(), JSON.stringify(ctx.renders));
    };
    step();
    step({ tick: 1 }); // same context values: no consumer renders
    step({ theme: 'dark' }); // the outer context changes: every Deep renders, no Row does
    step({ tick: 2 });
    step({ other: 2 }); // Deep 150 reads its own provider's 7
    step({ theme: 'dark', other: 2 }); // the same values again
    step({ theme: 'light', tick: 3 });
    assert.deepEqual(ctx.errors, []);
    results.push(log);
  }
  const [patched, published] = results;
  assert.deepEqual(patched, published);
  // The expected renders themselves (the same for both).
  const last = JSON.parse(patched[patched.length - 1]);
  assert.equal(last.row, 300, 'memo rows render once, at mount');
  assert.equal(last.app, 7);
  assert.equal(last.deeptop, 7, 'not memoized: renders with App');
  assert.equal(last.deep5, 4, 'mount, theme dark, other 2, theme light');
  assert.equal(last.deep296, 4);
  assert.equal(last.deep150, 4, 'React re-renders it for other 2 too (it reads 7)');
  const tree = patched[patched.length - 2];
  assert.match(tree, /\|5:light:2\|/);
  assert.match(tree, /\|150:light:7\|/);
  assert.match(tree, /\|tick3\|/);
});

test('scheduler: every bundled build yields on the runtime frame budget', async () => {
  for (const name of ['scheduler.native.production.js', 'scheduler.native.development.js', 'scheduler.production.js', 'scheduler.development.js']) {
    const file = require.resolve('scheduler/cjs/' + name, { paths: [jsuiReactDir()] });
    const { code, ok } = patchScheduler(await readFile(file, 'utf8'));
    assert.ok(ok, name);
    // shouldYieldToHost asks __jsui_native.shouldYield first, and keeps its
    // own clock without the bridge.
    const fn = code.slice(code.indexOf('function shouldYieldToHost()'));
    assert.match(fn.slice(0, 400), /jsui\.shouldYield\(\)/, name);
  }
});
