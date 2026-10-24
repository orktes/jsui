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
import { attach, bundleSource, compileBundle, dev, isPrecompiled, readPrecompiled, launch, findWasmDir, parseInlineSteps, parseSelector, SKILL_DIR } from '../src/index.js';
import { loadModule } from '../src/headless.js';
import { LiveApp } from '../src/shared.js';

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

test('run: steps as command-line words', () => {
  assert.deepEqual(parseInlineSteps(['press', 'Down', 'click', '#play', 'type', '42', 'key', 'Enter', 'wait', '200']),
    [{ press: 'Down' }, { click: '#play' }, { type: '42' }, { key: 'Enter' }, { wait: 200 }]);
  assert.deepEqual(parseInlineSteps(['expect', '#gold', 'text=99 gold', 'state', 'gold', 'value=99', 'press', 'A', 'count=3']),
    [{ expect: '#gold', text: '99 gold' }, { state: 'gold', value: 99 }, { press: 'A', count: 3 }]);
  assert.deepEqual(parseInlineSteps(['tree', 'reload', 'idle', 'idle', '1000', 'activity', 'click', '[10,20]', 'drag', '#k', 'to=[4,5]']),
    [{ tree: true }, { reload: true }, { idle: 5000 }, { idle: 1000 }, { activity: 0 }, { click: [10, 20] }, { drag: '#k', to: [4, 5] }]);
  assert.deepEqual(parseInlineSteps(['{"answer":"x","value":1}', '[{"wait":1},{"wait":2}]']), [{ answer: 'x', value: 1 }, { wait: 1 }, { wait: 2 }]);
  assert.throws(() => parseInlineSteps(['bogus']), /unknown step word/);
  assert.throws(() => parseInlineSteps(['click']), /needs a value/);
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

// Node has no Web Audio: the headless runtime is made with audio off, so
// sounds end 'failed' at once, preloads reject, and nothing throws.
test('launch: audio is off headless (voices fail, preloads reject, no errors)', { skip }, async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-headless-audio-'));
  await writeFile(path.join(dir, 'sound.jsx'), `import React, { useEffect } from 'react';
import { Text } from 'react-native';
import { Host, Sound, UISounds, useMusic } from '@orktes/jsui-react';
export default function Screen() {
  const music = useMusic('music.ogg', { fadeIn: 500 });
  useEffect(() => {
    Sound.play('tick.wav', { bus: 'ui' }).ended.then((e) => Host.postMessage('ended', e.reason));
    Sound.preload('tick.wav').then(() => Host.postMessage('preload', 'ok'), () => Host.postMessage('preload', 'rejected'));
    Sound.setBusVolume('music', 0.5, { fade: 200 });
    Sound.stopAll({ fade: 100 });
  }, []);
  return <UISounds sounds={{ press: 'tick.wav' }}><Text testID="t">sound</Text></UISounds>;
}
`);
  const app = await launch({ entry: path.join(dir, 'sound.jsx'), width: 100, height: 100 });
  try {
    await app.ready();
    await app.wait(50);
    const got = Object.fromEntries(app.messages.map((m) => [m.name, m.data]));
    assert.deepEqual(got, { ended: 'failed', preload: 'rejected' });
    assert.deepEqual(app.errors, []);
  } finally {
    app.close();
  }
});

test('jsui build precompiles to bytecode; the runtime runs it', { skip }, async () => {
  const dir = await fixture();
  await writeFile(path.join(dir, 'reg.jsx'), `import { AppRegistry, View } from 'react-native';
AppRegistry.registerComponent('main', () => () => <View style={{ flex: 1, backgroundColor: '#123456' }} />);
`);
  const out = path.join(dir, 'out', 'bundle.js');
  const r = await exec(process.execPath, [BIN, 'build', path.join(dir, 'reg.jsx'), '-o', out, '--minify']);
  assert.match(r.stdout, /bytecode \+ [\d.]+ KiB source/);
  const bytes = await readFile(out);
  assert.ok(isPrecompiled(bytes));
  assert.match(bundleSource(bytes), /registerComponent/);
  const shot = path.join(dir, 'b.png');
  await exec(process.execPath, [BIN, 'screenshot', out, '--bundle', '-o', shot, '--width', '64', '--height', '32']);
  assert.deepEqual(firstPixel(await readFile(shot)), [0x12, 0x34, 0x56, 255]);

  const plain = path.join(dir, 'out', 'plain.js');
  await exec(process.execPath, [BIN, 'build', path.join(dir, 'reg.jsx'), '-o', plain, '--format', 'source']);
  const src = await readFile(plain);
  assert.ok(!isPrecompiled(src));
  assert.equal(bundleSource(src), src.toString('utf8'));

  // Without the source fallback the bytecode alone runs (on QuickJS; the
  // host JS engine has nothing to run and says so).
  const lean = path.join(dir, 'out', 'lean.js');
  await writeFile(lean, await compileBundle(src, { keepSource: false }));
  assert.equal(bundleSource(await readFile(lean)), '');
  const leanShot = exec(process.execPath, [BIN, 'screenshot', lean, '--bundle', '-o', shot, '--width', '64', '--height', '32']);
  if (process.env.JSUI_JS === 'host') {
    await assert.rejects(leanShot, /no source for the host JS engine/);
  } else {
    await leanShot;
    assert.deepEqual(firstPixel(await readFile(shot)), [0x12, 0x34, 0x56, 255]);
  }

  // split: bytecode without the source, which sits next to it.
  const split = path.join(dir, 'out', 'split.js');
  const rs = await exec(process.execPath, [BIN, 'build', path.join(dir, 'reg.jsx'), '-o', split, '--format', 'split']);
  assert.match(rs.stdout, /source in split\.source\.js/);
  const parts = readPrecompiled(await readFile(split));
  assert.equal(parts.sourceUrl, 'split.source.js');
  assert.equal(parts.source, '');
  assert.match(await readFile(path.join(dir, 'out', 'split.source.js'), 'utf8'), /registerComponent/);
  await exec(process.execPath, [BIN, 'screenshot', split, '--bundle', '-o', shot, '--width', '64', '--height', '32']);
  assert.deepEqual(firstPixel(await readFile(shot)), [0x12, 0x34, 0x56, 255]);

  // bytecode only.
  const only = path.join(dir, 'out', 'only.js');
  await exec(process.execPath, [BIN, 'build', path.join(dir, 'reg.jsx'), '-o', only, '--format', 'bytecode']);
  assert.equal(bundleSource(await readFile(only)), '');

  await assert.rejects(exec(process.execPath, [BIN, 'build', path.join(dir, 'reg.jsx'), '-o', only, '--format', 'nope']), /unknown bundle format/);
  await assert.rejects(compileBundle('let x = ;', { name: 'broken.js' }), /SyntaxError[\s\S]*broken\.js/);
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

// Reads the shared view's stream until `n` messages arrived.
async function readStream(origin, n) {
  const res = await fetch(origin + '/__jsui/shared/stream');
  const reader = res.body.getReader();
  const out = [];
  let buf = Buffer.alloc(0);
  while (out.length < n) {
    const { value, done } = await reader.read();
    if (done) break;
    buf = Buffer.concat([buf, Buffer.from(value)]);
    while (buf.length >= 5 && buf.length - 4 >= buf.readUInt32LE(0)) {
      const len = buf.readUInt32LE(0);
      const type = buf[4];
      const payload = buf.subarray(5, 4 + len);
      out.push(type === 2 ? JSON.parse(payload.toString()) : { type: 'frame', w: payload.readUInt16LE(8), h: payload.readUInt16LE(10), pixels: zlib.inflateRawSync(payload.subarray(12)) });
      buf = buf.subarray(4 + len);
    }
  }
  await reader.cancel();
  return out;
}

test('dev --headless: viewer stream, viewer input, attach(), jsui run --attach', { skip }, async () => {
  const dir = await fixture();
  // The title lives in a component-only module, which hot-updates in place
  // (an edit to the entry file, which registers the app, reloads instead).
  const TITLE = `import { Text } from 'react-native';
export default function Title({ text }) { return <Text testID="title" style={{ color: '#fff', fontSize: 32 }}>{text}</Text>; }
`;
  await writeFile(path.join(dir, 'title.jsx'), TITLE);
  await writeFile(path.join(dir, 'app.jsx'), "import Title from './title.jsx';\n" +
    APP.replace(`<Text testID="title" style={{ color: '#fff', fontSize: 32 }}>{title}</Text>`, '<Title text={title} />'));
  const server = await dev({ entry: path.join(dir, 'app.jsx'), port: 0, log: () => {}, headless: { width: 640, height: 360 } });
  const origin = `http://127.0.0.1:${server.port}`;
  try {
    const page = await fetch(origin + '/shared');
    assert.equal(page.status, 200);
    assert.match(await page.text(), /JSUI shared session/);

    const app = await attach(origin);
    assert.equal(app.width, 640);
    await app.idle(3000);
    assert.equal((await app.find('#title')).text, 'Menu');

    // A viewer gets a hello and a full first frame.
    const [hello, first] = await readStream(origin, 2);
    assert.equal(hello.type, 'hello');
    assert.equal(hello.pixelWidth, 640);
    assert.equal(first.type, 'frame');
    assert.equal(first.w, 640);
    assert.equal(first.pixels.length, 640 * 360 * 4);

    // The person's input reaches the same app.
    const title = await app.get('#play');
    const at = { x: title.rect.x + 10, y: title.rect.y + 10 };
    const post = (body) => fetch(origin + '/__jsui/shared/input', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    assert.equal((await post([{ t: 'button', ...at, button: 0, down: true, mods: 0 }])).status, 204);
    await app.wait(60);
    await post([{ t: 'button', ...at, button: 0, down: false, mods: 0 }]);
    await app.wait(60);
    assert.deepEqual(app.messages.map((m) => m.name), ['play']);
    const activity = await app.activity(0);
    assert.ok(activity.some((e) => e.who === 'user' && e.kind === 'click'));
    assert.ok(activity.some((e) => e.who === 'ui' && e.kind === 'message' && e.name === 'play'));

    // Other pages cannot drive it (no JSON content type / foreign Host).
    assert.equal((await fetch(origin + '/__jsui/shared/input', { method: 'POST', body: '[]' })).status, 415);

    // The CLI attaches with the same steps as `jsui run`.
    const steps = path.join(dir, 'steps.json');
    await writeFile(steps, JSON.stringify({
      calls: { 'shop.buy': { value: { gold: 7 } } },
      steps: [
        { state: 'gold', value: 42 },
        { expect: '#gold', text: '42 gold' },
        { click: '#buy' },
        { wait: 100 },
        { expect: '#bought', text: 'ok 7' },
        { screenshot: 'shot.png' },
      ],
    }));
    const r = await exec(process.execPath, [BIN, 'run', '--attach', origin, steps], { cwd: dir });
    const lines = r.stdout.trim().split('\n').map((l) => JSON.parse(l));
    assert.ok(lines.at(-1).ok, r.stdout);
    assert.equal(pngSize(await readFile(path.join(dir, 'shot.png'))).width, 640);
    const words = await exec(process.execPath, [BIN, 'run', '--attach', origin, 'patch', 'settings', 'value={"volume":0.25}', 'expectState', 'settings', 'match={"volume":0.25}']);
    assert.ok(JSON.parse(words.stdout.trim().split('\n').at(-1)).ok, words.stdout);
    const shot = await exec(process.execPath, [BIN, 'screenshot', '--attach', origin, '-o', path.join(dir, 's2.png')]);
    assert.match(shot.stdout, /wrote/);
    const tree = await exec(process.execPath, [BIN, 'inspect', '--attach', origin, '--find', '#gold']);
    assert.match(tree.stdout, /"42 gold"/);
    const log = await exec(process.execPath, [BIN, 'log', '--attach', origin]);
    assert.match(log.stdout, /"who":"agent"/);

    // A save hot-updates the live app in place: focus and state survive.
    await app.click('#play');
    await app.press('Down');
    assert.ok((await app.find('#buy')).focused);
    const since = (await app.activity(0)).at(-1).seq;
    await writeFile(path.join(dir, 'title.jsx'), TITLE.replace('{text}', "{text + ' (edited)'}"));
    for (let i = 0; i < 100 && (await app.find('#title')).text !== 'Menu (edited)'; i++) await app.wait(100);
    assert.equal((await app.find('#title')).text, 'Menu (edited)');
    // A runtime older than hot updates (a pre-0.4 jsui.wasm) reloads instead;
    // CI builds the current one (JSUI_HEADLESS_STRICT).
    const reloaded = (await app.activity(since)).some((e) => e.kind === 'log' && /reloading/.test(e.text));
    if (process.env.JSUI_HEADLESS_STRICT || !reloaded) {
      assert.ok(!reloaded, 'hot update, not a reload');
      assert.ok((await app.find('#buy')).focused, 'focus kept across the hot update');
    }

    // Reload: fresh component state, shared state kept.
    await app.reload();
    assert.equal((await app.find('#bought')).text, '');
    assert.equal((await app.find('#gold')).text, '42 gold');
  } finally {
    await server.close();
  }
});

// The runtime's own session client (src/dev_session.cpp), as every dev-mode
// host runs it: here the WebAssembly build in Node, loading from the server.
test('dev: a runtime in dev mode joins the session; agents drive it, its input is reported', { skip }, async () => {
  const dir = await fixture();
  const server = await dev({ entry: path.join(dir, 'app.jsx'), port: 0, log: () => {} });
  const origin = `http://127.0.0.1:${server.port}`;
  const M = await loadModule();
  if (!M._jsui_set_dev_session) {
    await server.close();
    if (process.env.JSUI_HEADLESS_STRICT) throw new Error('jsui.wasm has no jsui_set_dev_session');
    return;  // a jsui.wasm older than the session client
  }
  const rt = new LiveApp(M, origin + '/bundle.js', { width: 640, height: 360, devSession: 'wasm test' });
  try {
    const app = await attach(origin);
    await app.idle(5000);
    assert.equal(app.runtime.label, 'wasm test (web)');
    assert.equal(app.width, 640);
    assert.equal((await app.find('#title')).text, 'Menu');

    await app.setState('gold', 42);
    await app.idle(2000);
    assert.equal((await app.find('#gold')).text, '42 gold');
    assert.equal(await app.getState('gold'), 42);
    await app.press('Down');
    assert.ok((await app.find('#buy')).focused);
    await app.click('#play');
    assert.deepEqual(app.messages.map((m) => m.name), ['play']);
    assert.equal(pngSize(await app.screenshot()).width, 640);

    // Input from the host itself (the person) is reported as theirs.
    const play = await app.get('#play');
    const x = play.rect.x + 5;
    const y = play.rect.y + 5;
    M._jsui_send_mouse_button(rt.rt, x, y, 0, 1, 0);
    await rt.wait(50);
    M._jsui_send_mouse_button(rt.rt, x, y, 0, 0, 0);
    let feed = [];
    for (let i = 0; i < 50 && !feed.some((e) => e.who === 'user' && e.kind === 'click'); i++) {
      await app.frame();
      feed = await app.activity(0);
    }
    const click = feed.find((e) => e.who === 'user' && e.kind === 'click');
    assert.ok(click, JSON.stringify(feed));
    assert.equal(click.client, 'wasm test (web)');
    assert.deepEqual([click.x, click.y, click.consumed], [Math.round(x), Math.round(y), true]);
    // The agent's own clicks are not.
    assert.equal(feed.filter((e) => e.who === 'user' && e.kind === 'click').length, 1);
    assert.ok(feed.some((e) => e.who === 'ui' && e.kind === 'message' && e.name === 'play'));

    // The CLI drives it the same way.
    const words = await exec(process.execPath, [BIN, 'run', '--attach', origin, 'patch', 'settings', 'value={"volume":0.25}', 'expectState', 'settings', 'match={"volume":0.25}']);
    assert.ok(JSON.parse(words.stdout.trim().split('\n').at(-1)).ok, words.stdout);
  } finally {
    await server.close();
    rt.close();
  }
});
