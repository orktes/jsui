// Hot module replacement: runs the `jsui dev` bundle in a bare VM context
// (a recording bridge, like the runtime's), edits files and applies the
// updates the dev server sends, checking that component state survives.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import { dev } from '../src/index.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function until(pred, what, ms = 8000) {
  const t0 = Date.now();
  for (;;) {
    const v = await pred();
    if (v) return v;
    if (Date.now() - t0 > ms) throw new Error('timed out waiting for ' + what);
    await sleep(20);
  }
}

function app(label, extra = '') {
  return `import React, { useState } from 'react';
import { View, Text } from 'react-native';
import Badge from './Badge';
import { greeting } from './strings';
export default function App() {
  const [n, setN] = useState(0);
  ${extra}
  globalThis.bump = () => setN((x) => x + 1);
  return (
    <View>
      <Text nativeID="title">{${JSON.stringify(label)} + ' ' + n}</Text>
      <Text nativeID="greeting">{greeting}</Text>
      <Badge />
    </View>
  );
}
`;
}

const badge = (label) => `import React, { useState } from 'react';
import { Text } from 'react-native';
export default function Badge() {
  const [hits, setHits] = useState(0);
  globalThis.hit = () => setHits((x) => x + 1);
  return <Text nativeID="badge">{${JSON.stringify(label)} + ' ' + hits}</Text>;
}
`;

async function fixture() {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-hmr-'));
  await writeFile(path.join(dir, 'App.js'), app('v1'));
  await writeFile(path.join(dir, 'Badge.js'), badge('b1'));
  await writeFile(path.join(dir, 'strings.js'), `export const greeting = 'hello';\n`);
  await writeFile(
    path.join(dir, 'index.js'),
    `import { AppRegistry } from 'react-native';
import App from './App';
AppRegistry.registerComponent('main', () => App);
`,
  );
  return dir;
}

/** The runtime's side of dev mode, in a VM context. */
function runtime(base) {
  const nodes = new Map([[1, { type: 'Root', props: {}, children: [], text: '' }]]);
  let next = 2;
  const ctx = {
    console: { log: (...a) => ctx.logs.push(a.join(' ')), warn() {}, error: (...a) => ctx.errors.push(a.join(' ')), info() {}, debug() {} },
    errors: [],
    logs: [],
    devErrors: [],
    reloads: 0,
    evalNames: [],
    setTimeout,
    clearTimeout,
    reportError: (e) => ctx.devErrors.push('Uncaught ' + e.message),
    fetch,
    URL,
    location: { href: base + '/bundle.js' },
    __jsui_native: {
      rootId: 1,
      devMode: true,
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
        const k = nodes.get(p).children;
        if (k.includes(c)) k.splice(k.indexOf(c), 1);
        k.push(c);
      },
      insertBefore(p, c, b) {
        const k = nodes.get(p).children;
        if (k.includes(c)) k.splice(k.indexOf(c), 1);
        k.splice(k.indexOf(b), 0, c);
      },
      removeChild(p, c) {
        const k = nodes.get(p).children;
        k.splice(k.indexOf(c), 1);
      },
      destroyNode() {},
      reload() {
        ctx.reloads++;
      },
      devError(text) {
        ctx.devErrors.push(text);
      },
      evalScript(code, name) {
        ctx.evalNames.push(name);
        return vm.runInContext(code, ctx, { filename: name });
      },
    },
  };
  ctx.globalThis = ctx;
  vm.createContext(ctx);
  // Searches the mounted tree (destroyed nodes stay in `nodes`).
  const find = (id, nativeID) => {
    const n = nodes.get(id);
    if (n.props.nativeID === nativeID) return n;
    for (const c of n.children) {
      const r = find(c, nativeID);
      if (r) return r;
    }
    return null;
  };
  const text = (nativeID) => {
    const n = find(1, nativeID);
    return n ? n.children.map((c) => nodes.get(c).text).join('') : null;
  };
  return { ctx, nodes, text };
}

test('hot updates keep component state; non-component edits propagate; entry edits reload', async () => {
  const dir = await fixture();
  const server = await dev({ entry: path.join(dir, 'index.js'), port: 0, log: () => {} });
  const base = `http://127.0.0.1:${server.port}`;
  try {
    const res = await fetch(base + '/bundle.js');
    let version = res.headers.get('x-jsui-version');
    const { ctx, text } = runtime(base);
    vm.runInContext(await res.text(), ctx, { filename: 'bundle.js' });
    await until(() => text('title') === 'v1 0', 'first render');
    assert.equal(ctx.__jsui_hmr.version, version);
    for (let i = 0; i < 3; i++) ctx.bump();
    ctx.hit();
    await until(() => text('title') === 'v1 3' && text('badge') === 'b1 1', 'state updates');

    // The runtime polls with hmr=1 and calls __jsui_onDevUpdate on a change.
    const edit = async (file, contents) => {
      await sleep(150);  // let duplicate fs events of the previous write settle
      const before = server.state.version;
      const failures = server.state.failures;
      await writeFile(path.join(dir, file), contents);
      await until(() => server.state.version !== before || server.state.failures !== failures, 'rebuild of ' + file);
      version = await (await fetch(base + '/__jsui/version?hmr=1')).text();
      ctx.__jsui_onDevUpdate(version);
    };

    // A component edit: re-rendered in place, both components keep state.
    await edit('App.js', app('v2'));
    await until(() => text('title') === 'v2 3', 'hot update of App');
    assert.equal(text('badge'), 'b1 1');
    assert.equal(ctx.reloads, 0);
    assert.equal(ctx.__jsui_hmr.version, server.state.version);
    // Debuggers list each update script by this name.
    assert.equal(ctx.evalNames.at(-1), `${base}/__jsui/hmr/${server.state.version}.js`);

    await edit('Badge.js', badge('b2'));
    await until(() => text('badge') === 'b2 1', 'hot update of Badge');
    assert.equal(text('title'), 'v2 3');

    // A module that exports data: its importer (a boundary) re-runs.
    await edit('strings.js', `export const greeting = 'hi there';\n`);
    await until(() => text('greeting') === 'hi there', 'propagated update');
    assert.equal(text('title'), 'v2 3');
    assert.equal(ctx.reloads, 0);

    // Changed hooks remount the component (React Refresh signatures).
    await edit('App.js', app('v3', 'const [m] = useState(5);'));
    await until(() => text('title') === 'v3 0', 'remount on hook change');
    assert.equal(ctx.reloads, 0);

    // A build error shows on the overlay; the fix clears it.
    await edit('App.js', `import React from 'react';\nexport default function App() { return <View>; }\n`);
    await until(() => ctx.devErrors.some((e) => typeof e === 'string' && /Build failed/.test(e) && /App\.js/.test(e)), 'build error overlay');
    ctx.bump();
    await edit('App.js', app('v4', 'const [m] = useState(5);'));
    await until(() => text('title') === 'v4 1', 'update after the fix');
    assert.equal(ctx.devErrors.at(-1), null, 'overlay cleared');

    // A runtime error while re-running a module shows on the overlay, and
    // the next save retries it.
    await edit('strings.js', `export const greeting = (() => { throw new Error('boom'); })();\n`);
    await until(() => ctx.devErrors.some((e) => typeof e === 'string' && /Hot update failed/.test(e) && /boom/.test(e)), 'runtime error overlay');
    await edit('strings.js', `export const greeting = 'fixed';\n`);
    await until(() => text('greeting') === 'fixed', 'retry after a runtime error');
    assert.equal(text('title'), 'v4 1');
    assert.equal(ctx.reloads, 0);

    // A render error unmounts the root (React 19); the fix mounts it again.
    await edit('Badge.js', `export default function Badge() { throw new Error('render boom'); }\n`);
    await until(() => ctx.devErrors.some((e) => /render boom/.test(e)), 'render error reported');
    await edit('Badge.js', badge('b3'));
    await until(() => text('badge') === 'b3 0', 'root mounted again after the fix');
    assert.equal(ctx.reloads, 0);
    assert.match(ctx.errors.join('\n'), /render boom/);
    ctx.errors.length = 0;

    // The entry module has no importers: nothing can accept it.
    await edit('index.js', `import { AppRegistry } from 'react-native';\nimport App from './App';\nAppRegistry.registerComponent('main', () => App);\n// edited\n`);
    await until(() => ctx.reloads === 1, 'full reload for an entry edit');
    assert.deepEqual(ctx.errors, []);

    assert.equal(new Set(ctx.evalNames).size, ctx.evalNames.length, 'update script names are unique');

    // Unknown versions (a restarted server) reload too.
    assert.match(await (await fetch(base + '/__jsui/hmr?from=nope')).text(), /__jsui_hmr\.reload\(/);
  } finally {
    await server.close();
  }
});

test('--no-hmr serves the plain bundle', async () => {
  const dir = await fixture();
  const server = await dev({ entry: path.join(dir, 'index.js'), port: 0, log: () => {}, hmr: false });
  const base = `http://127.0.0.1:${server.port}`;
  try {
    const code = await (await fetch(base + '/bundle.js')).text();
    assert.doesNotMatch(code, /__jsui_hmr/);
    assert.equal((await fetch(base + '/bundle.js.map')).status, 200);
    assert.match(await (await fetch(base + '/__jsui/hmr?from=x')).text(), /reload/);
  } finally {
    await server.close();
  }
});

// React Compiler memoizes the JSX on `n` only (LABEL is a module constant);
// a hot update must not serve the previous version's cached elements (the compiler's source hash check
// resets the memo cache), and state still survives.
test('hot updates with React Compiler reset its memo caches and keep state', async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-hmr-rc-'));
  const counter = (label) => `import React, { useEffect, useState } from 'react';
import { View, Text } from 'react-native';
const LABEL = ${JSON.stringify(label)};
export default function App() {
  const [n, setN] = useState(0);
  useEffect(() => {
    globalThis.bump = () => setN((x) => x + 1);
  }, []);
  return (
    <View>
      <Text nativeID="title">{LABEL}{' '}{n}</Text>
    </View>
  );
}
`;
  await writeFile(path.join(dir, 'App.js'), counter('v1'));
  await writeFile(path.join(dir, 'index.js'), `import { AppRegistry } from 'react-native';\nimport App from './App';\nAppRegistry.registerComponent('main', () => App);\n`);
  const server = await dev({ entry: path.join(dir, 'index.js'), port: 0, log: () => {}, reactCompiler: true });
  const base = `http://127.0.0.1:${server.port}`;
  try {
    const code = await (await fetch(base + '/bundle.js')).text();
    assert.match(code, /useMemoCache/);
    const { ctx, text } = runtime(base);
    vm.runInContext(code, ctx, { filename: 'bundle.js' });
    await until(() => text('title') === 'v1 0', 'first render');
    ctx.bump();
    ctx.bump();
    await until(() => text('title') === 'v1 2', 'state update');
    await sleep(150);
    const before = server.state.version;
    await writeFile(path.join(dir, 'App.js'), counter('v2'));
    await until(() => server.state.version !== before, 'rebuild');
    await sleep(300);  // let duplicate fs events settle
    // The updated module checks its source hash against the memo cache's.
    const update = await (await fetch(base + '/__jsui/hmr?from=' + ctx.__jsui_hmr.version)).text();
    assert.match(update, /if \(\$\[0\] !== "[0-9a-f]{64}"\)/);
    ctx.__jsui_onDevUpdate(await (await fetch(base + '/__jsui/version?hmr=1')).text());
    await until(() => text('title') === 'v2 2', 'hot update past the memo cache');
    assert.equal(ctx.reloads, 0);
    assert.deepEqual(ctx.errors, []);
  } finally {
    await server.close();
  }
});
