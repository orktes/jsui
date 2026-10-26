// The worklets transform (src/worklets.js, Software Mansion's Babel plugin):
// what it rewrites, the init data it emits per mode, TypeScript, and that
// jsui build / jsui dev run it.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { build, dev } from '../src/index.js';
import { mayHaveWorklets, prepareSource, transformWorklets } from '../src/worklets.js';

const initData = (code) => [...code.matchAll(/_init_data = (\{[^\n]*?\});/g)].map((m) => Function(`return ${m[1]}`)());

test('worklet directives and runOnUI callbacks become worklets', async () => {
  const src = [
    "import { runOnUI, runOnJS } from 'react-native-worklets';",
    'const K = 3;',
    "function sq(x) { 'worklet'; return x * x + K; }",
    'export function go(report) {',
    '  runOnUI((a) => { runOnJS(report)(sq(a)); })(2);',
    '}',
  ].join('\n');
  const file = path.join(process.cwd(), 'src', 'app.jsx');
  const r = await transformWorklets(src, file, { mode: 'development' });
  assert.equal(r.changed, true);
  assert.equal(r.loader, 'jsx');
  assert.match(r.code, /sq\.__closure = \[K\]/);
  // (runOnJS too: the runtime gives a worklet its own copy of a builtin.)
  assert.match(r.code, /appJsx2.__closure = \[runOnJS, report, sq\]/);
  const data = initData(r.code);
  assert.equal(data.length, 2);
  assert.match(data[0].code, /^\(function sq_appJsx1\(x\)\{const\[K\]=this\.__closure;return x\*x\+K;\}\)$/);
  assert.equal(data[0].location, 'src/app.jsx');
  assert.equal(data[0].line, 3);
  assert.equal(data[1].line, 5);
  // The worklet code runs on its own: a function of its closure.
  const sq = Function(`return ${data[0].code}`)().bind({ __closure: [3] });
  assert.equal(sq(4), 19);
  // The rewritten module still evaluates (imports aside).
  new Function(r.code.replace(/^import .*$/m, '').replace(/export /, ''));
});

test('production leaves out locations and dev stack data', async () => {
  const src = "function f() { 'worklet'; return 1; }";
  const r = await transformWorklets(src, '/x/app.js', { mode: 'production' });
  assert.equal(r.changed, true);
  assert.doesNotMatch(r.code, /location|__stackDetails|__pluginVersion/);
  assert.deepEqual(Object.keys(initData(r.code)[0]), ['code']);
});

test('TypeScript is stripped first and lines point at the source', async () => {
  const src = [
    'type N = number;',
    '',
    'const K: N = 2;',
    "export function f(x: N): N { 'worklet'; return (x as number) * K; }",
  ].join('\n');
  const r = await transformWorklets(src, path.join(process.cwd(), 'app.ts'), { mode: 'development' });
  assert.equal(r.loader, 'jsx');
  const [d] = initData(r.code);
  assert.doesNotMatch(d.code, /: N|as number/);
  assert.equal(d.line, 4);
});

test('files that only mention the word stay as written', async () => {
  assert.equal(mayHaveWorklets('const a = 1;'), false);
  const src = "// a worklet would go here\nconst x = 'worklet';\n";
  const r = await transformWorklets(src, '/x/a.js');
  assert.deepEqual(r, { code: src, changed: false });
  const p = await prepareSource('const a: number = 1;', '/x/a.ts');
  assert.equal(p.changed, false);
  assert.equal(p.loader, 'ts');
});

async function app() {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-worklets-'));
  await writeFile(
    path.join(dir, 'index.js'),
    `import { AppRegistry, View } from 'react-native';
import { runOnUI, getRuntimeKind } from 'react-native-worklets';
const App = () => { runOnUI(() => { console.log(getRuntimeKind()); })(); return <View />; };
AppRegistry.registerComponent('main', () => App);
`,
  );
  return dir;
}

test('jsui build and jsui dev workletize the app and alias the package', async () => {
  const dir = await app();
  const outfile = path.join(dir, 'out.js');
  for (const mode of ['production', 'development']) {
    const r = await build({ entry: path.join(dir, 'index.js'), outfile, mode, format: 'source' });
    const code = r && r.code ? r.code : (await import('node:fs')).readFileSync(outfile, 'utf8');
    assert.match(code, /__workletHash/, mode);
    assert.match(code, /__jsui_onWorklets/, mode);  // @orktes/jsui-react's implementation
  }
  const server = await dev({ entry: path.join(dir, 'index.js'), port: 0, log: () => {} });
  try {
    const code = await (await fetch(`http://127.0.0.1:${server.port}/bundle.js`)).text();
    assert.match(code, /__workletHash/);
  } finally {
    await server.close();
  }
});
