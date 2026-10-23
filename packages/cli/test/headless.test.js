// Headless runtime (jsui screenshot / inspect / run, launch()) and the agent
// skill. The headless tests need the CPU WebAssembly module (web/dist, or
// JSUI_WASM_DIR); without it they are skipped unless JSUI_HEADLESS_STRICT=1
// (CI's wasm job, which builds it).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile, readdir, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { launch, findWasmDir, parseSelector, SKILL_DIR } from '../src/index.js';

const exec = promisify(execFile);
const BIN = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'bin', 'jsui.js');
const WASM = findWasmDir();
if (!WASM && process.env.JSUI_HEADLESS_STRICT) throw new Error('JSUI_HEADLESS_STRICT: no jsui.wasm (build web/dist or set JSUI_WASM_DIR)');
const skip = WASM ? false : 'no CPU jsui.wasm (scripts/build-wasm.sh -> web/dist, or JSUI_WASM_DIR)';

const APP = `import React, { useState } from 'react';
import { AppRegistry, Pressable, Text, TextInput, View } from 'react-native';
import { Host, HostState, useHostState, useHostHandler, useHostBufferSelector } from '@orktes/jsui-react';

function Button({ id, label, onPress, first }) {
  return (
    <Pressable testID={id} hasTVPreferredFocus={first} onPress={onPress}
      style={({ focused }) => ({ padding: 12, width: 200, backgroundColor: focused ? '#f2c14e' : '#223' })}>
      <Text style={{ color: '#fff' }}>{label}</Text>
    </Pressable>
  );
}

function App({ title = 'Menu' }) {
  const gold = useHostState('gold', 0);
  const enemies = useHostBufferSelector('enemies', (b) => b.length / 2, 0);
  const [name, setName] = useState('');
  const [bought, setBought] = useState('');
  const [toast, setToast] = useState('');
  useHostHandler('ui.toast', ({ text }) => { setToast(text); return { shown: true }; });
  return (
    <View style={{ flex: 1, padding: 40, gap: 8, backgroundColor: '#0b0f1a' }}>
      <Text testID="title" style={{ color: '#fff', fontSize: 32 }}>{title}</Text>
      <Text testID="gold" style={{ color: '#fc0' }}>{gold} gold</Text>
      <Text testID="enemies" style={{ color: '#fff' }}>{enemies} enemies</Text>
      <Button id="play" first label="Play" onPress={() => Host.postMessage('play', { at: 1 })} />
      <Button id="buy" label="Buy" onPress={() => Host.call('shop.buy', { item: 'potion' }).then((r) => setBought('ok ' + r.gold), (e) => setBought('err ' + e.message))} />
      <Button id="save" label="Save" onPress={() => HostState.set('settings', { volume: 0.5 })} />
      <TextInput testID="name" value={name} onChangeText={setName} placeholder="Callsign" style={{ color: '#fff', width: 200, height: 32 }} />
      <Text testID="echo" style={{ color: '#fff' }}>{name}</Text>
      <Text testID="bought" style={{ color: '#fff' }}>{bought}</Text>
      <Text testID="toast" style={{ color: '#fff' }}>{toast}</Text>
    </View>
  );
}
AppRegistry.registerComponent('main', () => App);
`;

async function fixture() {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-headless-test-'));
  await writeFile(path.join(dir, 'app.jsx'), APP);
  // A screen module that only exports a component (registered as 'main').
  await writeFile(path.join(dir, 'screen.jsx'), `import { View, Text } from 'react-native';
export default function Screen() { return <View style={{ flex: 1, backgroundColor: '#123456' }}><Text testID="hi">Hi</Text></View>; }
`);
  return dir;
}

function pngSize(buf) {
  assert.equal(buf.subarray(1, 4).toString(), 'PNG');
  return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) };
}

// First pixel of a PNG (8-bit RGBA, filter byte per row).
function firstPixel(buf) {
  const chunks = [];
  for (let o = 8; o < buf.length; ) {
    const len = buf.readUInt32BE(o);
    if (buf.toString('latin1', o + 4, o + 8) === 'IDAT') chunks.push(buf.subarray(o + 8, o + 8 + len));
    o += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(chunks));
  return [...raw.subarray(1, 5)];
}

test('selectors', () => {
  assert.deepEqual(parseSelector('#play'), { testID: 'play' });
  assert.deepEqual(parseSelector('text=Play'), { text: 'Play' });
  assert.deepEqual(parseSelector('Pla >> 1'), { textContains: 'Pla', index: 1 });
  assert.deepEqual(parseSelector('type=TextInput'), { type: 'TextInput' });
  assert.deepEqual(parseSelector({ label: 'x' }), { label: 'x' });
});

test('launch: render, inspect, focus, input, state, buffers, calls', { skip }, async () => {
  const dir = await fixture();
  const app = await launch({ entry: path.join(dir, 'app.jsx'), width: 400, height: 600, scale: 2, state: { gold: 7 },
    props: { title: 'Hello' }, calls: { 'shop.buy': ({ item }) => ({ gold: item.length }) } });
  try {
    await app.ready();
    assert.deepEqual(pngSize(await app.screenshot()), { width: 800, height: 1200 });
    assert.equal((await app.get('#title')).text, 'Hello');
    assert.equal((await app.get('#gold')).text, '7 gold');
    assert.equal((await app.get('#play')).focused, true);
    assert.equal((await app.get('#play')).pressable, true);
    assert.equal((await app.findAll('Play')).length, 1);

    assert.equal(await app.press('Down'), true);
    assert.equal((await app.get('#buy')).focused, true);
    assert.equal(await app.press('A'), true);
    await app.wait(50);
    assert.equal((await app.get('#bought')).text, 'ok 6');
    assert.deepEqual(app.calls.map((c) => c.name), ['shop.buy']);

    assert.equal(await app.click('#play'), true);
    assert.deepEqual(app.messages.at(-1), { name: 'play', data: { at: 1 }, time: app.messages.at(-1).time });

    await app.click('#save');
    assert.deepEqual(app.getState('settings'), { volume: 0.5 });
    assert.equal(app.stateWrites.at(-1).key, 'settings');

    app.setState('gold', 99);
    app.setBuffer('enemies', [1, 2, 3, 4, 5, 6]);
    await app.frame();
    assert.equal((await app.get('#gold')).text, '99 gold');
    assert.equal((await app.get('#enemies')).text, '3 enemies');

    await app.click('#name');
    await app.type('Nova');
    assert.equal((await app.get('#echo')).text, 'Nova');
    assert.equal((await app.get('type=TextInput')).value, 'Nova');

    assert.deepEqual(await app.callUi('ui.toast', { text: 'Saved' }), { shown: true });
    await app.frame();
    assert.equal((await app.get('#toast')).text, 'Saved');

    assert.equal((await app.hitTest('#play')).pointer, true);
    assert.equal((await app.hitTest([399, 599])).pointer, false);
    assert.equal(await app.find('#nope'), null);
    await assert.rejects(app.get('#nope'), /no element matches/);
    assert.deepEqual(app.errors, []);
  } finally {
    app.close();
  }
});

test('cli: screenshot, inspect, run (json + js), default-export screens', { skip }, async () => {
  const dir = await fixture();
  const shot = path.join(dir, 'shots', 'a.png');
  await exec(process.execPath, [BIN, 'screenshot', path.join(dir, 'screen.jsx'), '-o', shot, '--width', '320', '--height', '200', '--wait', '100']);
  const png = await readFile(shot);
  assert.deepEqual(pngSize(png), { width: 320, height: 200 });
  assert.deepEqual(firstPixel(png), [0x12, 0x34, 0x56, 255]);

  const tree = await exec(process.execPath, [BIN, 'inspect', path.join(dir, 'app.jsx'), '--state', 'gold=5']);
  assert.match(tree.stdout, /Text #gold "5 gold"/);
  assert.match(tree.stdout, /View #play \[pressable focused\]/);
  const found = await exec(process.execPath, [BIN, 'inspect', path.join(dir, 'app.jsx'), '--find', '#buy', '--json']);
  assert.equal(JSON.parse(found.stdout)[0].testID, 'buy');

  const steps = path.join(dir, 'steps.json');
  await writeFile(steps, JSON.stringify([
    { press: 'Down' },
    { expect: '#buy', focused: true },
    { click: '#play' },
    { expectMessage: 'play' },
    { state: 'gold', value: 3 },
    { expect: '#gold', text: '3 gold' },
    { click: '#save' },
    { expectState: 'settings', value: { volume: 0.5 } },
    { screenshot: path.join(dir, 'run.png') },
  ]));
  const run = await exec(process.execPath, [BIN, 'run', path.join(dir, 'app.jsx'), steps]);
  const lines = run.stdout.trim().split('\n').map((l) => JSON.parse(l));
  assert.equal(lines.length, 10);
  assert.ok(lines.slice(0, 9).every((l) => l.ok), run.stdout);
  assert.equal(lines[9].ok, true);
  assert.ok((await stat(path.join(dir, 'run.png'))).size > 0);

  await writeFile(steps, JSON.stringify([{ expect: '#missing' }, { click: '#play' }]));
  const failed = await exec(process.execPath, [BIN, 'run', path.join(dir, 'app.jsx'), steps]).catch((e) => e);
  assert.equal(failed.code, 1);
  const out = failed.stdout.trim().split('\n').map((l) => JSON.parse(l));
  assert.equal(out[0].ok, false);
  assert.equal(out.length, 2);  // stopped after the failure, then the summary

  const script = path.join(dir, 'script.mjs');
  await writeFile(script, `export default async function (app) {
  app.handle('shop.buy', () => ({ gold: 42 }));
  await app.click('#buy');
  await app.wait(50);
  return (await app.get('#bought')).text;
}`);
  const js = await exec(process.execPath, [BIN, 'run', path.join(dir, 'app.jsx'), script]);
  assert.equal(JSON.parse(js.stdout).result, 'ok 42');
});

test('run: answers, partial state match, text inside views', { skip }, async () => {
  const dir = await fixture();
  const steps = path.join(dir, 'answers.json');
  await writeFile(steps, JSON.stringify({
    calls: { 'shop.buy': { error: 'sold out' } },
    steps: [
      { expect: '#play', text: 'Play' },
      { click: '#buy' }, { wait: 100 }, { expect: '#bought', text: 'err sold out' },
      { answer: 'shop.buy', value: { gold: 12 } },
      { click: '#buy' }, { wait: 100 }, { expect: '#bought', textContains: 'ok 12' },
      { state: 'settings', value: { volume: 1, lang: 'fi' } },
      { expectState: 'settings', match: { lang: 'fi' } },
    ],
  }));
  const run = await exec(process.execPath, [BIN, 'run', path.join(dir, 'app.jsx'), steps]);
  const lines = run.stdout.trim().split('\n').map((l) => JSON.parse(l));
  assert.ok(lines.every((l) => l.ok), run.stdout);
  assert.ok(lines.at(-1).calls.every((c) => Number.isInteger(c.time)));
});

test('cli: JS errors fail the command', { skip }, async () => {
  const dir = await fixture();
  await writeFile(path.join(dir, 'bad.jsx'), `import { View } from 'react-native';
export default function Bad() { throw new Error('boom from render'); }`);
  const r = await exec(process.execPath, [BIN, 'screenshot', path.join(dir, 'bad.jsx'), '-o', path.join(dir, 'bad.png')]).catch((e) => e);
  assert.equal(r.code, 1);
  assert.match(r.stderr, /boom from render/);
});

test('skill: SKILL.md frontmatter, references resolve, print and install', async () => {
  const md = await readFile(path.join(SKILL_DIR, 'SKILL.md'), 'utf8');
  assert.match(md, /^---\nname: jsui\ndescription: .+\n---\n/);
  for (const [, link] of md.matchAll(/\]\((references\/[^)]+)\)/g)) await stat(path.join(SKILL_DIR, link));
  const refs = await readdir(path.join(SKILL_DIR, 'references'));
  for (const f of refs) assert.ok(md.includes(`references/${f}`), `SKILL.md links ${f}`);

  const printed = await exec(process.execPath, [BIN, 'skill']);
  assert.equal(printed.stdout, md);
  const all = await exec(process.execPath, [BIN, 'skill', 'print', '--all']);
  assert.ok(all.stdout.includes('<!-- references/testing-tools.md -->'));

  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-skill-'));
  await exec(process.execPath, [BIN, 'skill', 'install', dir]);
  assert.equal(await readFile(path.join(dir, '.claude', 'skills', 'jsui', 'SKILL.md'), 'utf8'), md);
  await stat(path.join(dir, '.claude', 'skills', 'jsui', 'references', 'testing-tools.md'));
  await exec(process.execPath, [BIN, 'skill', 'install', dir]);  // same content: fine
  await writeFile(path.join(dir, '.claude', 'skills', 'jsui', 'SKILL.md'), 'local edits');
  const refused = await exec(process.execPath, [BIN, 'skill', 'install', dir]).catch((e) => e);
  assert.equal(refused.code, 1);
  await exec(process.execPath, [BIN, 'skill', 'install', dir, '--force']);
  assert.equal(await readFile(path.join(dir, '.claude', 'skills', 'jsui', 'SKILL.md'), 'utf8'), md);
});
