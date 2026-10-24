import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import { build } from '../src/build.js';

// ESM imports of react / react/jsx-runtime go through a facade of plain
// bindings (config.js): named imports are direct, React's default export is
// module.exports as before, and a name React does not export still builds
// and reads as undefined.
test('react ESM facade: direct bindings, default export, unknown names', async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-facade-'));
  await writeFile(
    path.join(dir, 'App.js'),
    `import React, { useState, unstable_doesNotExist } from 'react';
import * as NS from 'react';
import { Text, View, render } from 'react-native';
function App() {
  const [s] = useState('ok');
  const facts = [s, typeof unstable_doesNotExist, String(React.useState === useState), String(NS.useState === useState), String(typeof NS.unstable_alsoMissing)];
  return <View><Text>{facts.join(',')}</Text></View>;
}
render(<App />);
`,
  );
  const out = path.join(dir, 'out', 'bundle.js');
  await build({ entry: path.join(dir, 'App.js'), outfile: out });
  const code = await readFile(out, 'utf8');
  assert.ok(!code.includes(path.resolve(import.meta.dirname, '../../..')), 'no absolute repo paths in the bundle');
  assert.match(code, /[\s=(,]useState\d*\("ok"\)/, "useState called through its binding, not (0, import_react.useState)");
  const texts = [];
  const ctx = {
    console: { log() {}, warn() {}, error: (...a) => texts.push('error ' + a.join(' ')), info() {}, debug() {} },
    setTimeout: () => 0,
    clearTimeout() {},
    __jsui_native: {
      rootId: 1,
      createNode: (() => {
        let n = 2;
        return () => n++;
      })(),
      setProps() {},
      setText: (_id, t) => texts.push(t),
      appendChild() {},
      insertBefore() {},
      removeChild() {},
      destroyNode() {},
    },
  };
  ctx.globalThis = ctx;
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  assert.deepEqual(texts, ['ok,undefined,true,true,undefined']);
});
