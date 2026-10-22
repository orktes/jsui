import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile, mkdir, readdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import zlib from 'node:zlib';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { build, dev, imageSize } from '../src/index.js';

const BIN = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'bin', 'jsui.js');

function png(w, h) {
  const crc = (buf) => {
    let c = ~0;
    for (const b of buf) {
      c ^= b;
      for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1;
    }
    return ~c >>> 0;
  };
  const chunk = (type, data) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const c = Buffer.alloc(4);
    c.writeUInt32BE(crc(td));
    return Buffer.concat([len, td, c]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  const raw = Buffer.alloc((w * 4 + 1) * h);
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

async function fixture() {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-cli-'));
  await writeFile(path.join(dir, 'logo@2x.png'), png(64, 32));
  await writeFile(path.join(dir, 'data.json'), JSON.stringify({ title: 'Héllo ✓' }));
  await writeFile(path.join(dir, 'font.ttf'), Buffer.from('fake font'));
  await writeFile(path.join(dir, 'platform.js'), `export default 'web';`);
  await writeFile(path.join(dir, 'platform.native.js'), `export default 'native';`);
  await writeFile(
    path.join(dir, 'App.js'),
    `import React from 'react';
import { AppRegistry, View, Text, Image, Platform } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import { LinearGradient } from 'expo-linear-gradient';
import data from './data.json';
import which from './platform';
import fontUrl from './font.ttf';
const logo = require('./logo@2x.png');
function App() {
  return (
    <SafeAreaView style={{ flex: 1 }}>
      <Text nativeID="title">{data.title} {which} {Platform.OS} {__DEV__ ? 'dev' : 'prod'} {fontUrl}</Text>
      <Image nativeID="logo" source={logo} />
      <LinearGradient nativeID="grad" colors={['red', 'blue']} />
    </SafeAreaView>
  );
}
AppRegistry.registerComponent('main', () => App);
`,
  );
  return dir;
}

/** Runs a bundle in a bare VM context with a recording bridge (no Node APIs). */
function runBundle(code) {
  const nodes = new Map([[1, { type: 'Root', props: {}, children: [], text: '' }]]);
  let next = 2;
  const timers = [];
  const ctx = {
    console: { log() {}, warn() {}, error: (...a) => ctx.errors.push(a.join(' ')), info() {}, debug() {} },
    errors: [],
    setTimeout: (fn) => timers.push(fn),
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
  return { ctx, nodes, timers };
}

test('build: RN alias, JSX in .js, assets, json, platform ext, shims; runs without Node APIs', async () => {
  const dir = await fixture();
  const out = path.join(dir, 'dist', 'bundle.js');
  const r = await build({ entry: path.join(dir, 'App.js'), outfile: out });
  const code = await readFile(out, 'utf8');
  assert.ok(!/^\s*(import|export)\s/m.test(code), 'no top-level ESM');
  assert.ok(!/\brequire\(["']react/.test(code), 'no runtime require');
  const assets = await readdir(path.join(dir, 'dist', 'assets'));
  assert.equal(assets.length, 2);
  assert.ok(assets.some((a) => /^logo@2x-[0-9a-f]{8}\.png$/.test(a)));
  assert.equal(r.assets.length, 2);

  const { nodes, ctx } = runBundle(code);
  await new Promise((res) => setImmediate(res));
  assert.deepEqual(ctx.errors, []);
  const all = [...nodes.values()];
  const title = all.find((n) => n.props.nativeID === 'title');
  const text = title.children.map((c) => nodes.get(c).text).join('');
  assert.match(text, /^Héllo ✓ native jsui prod assets\/font-[0-9a-f]{8}\.ttf$/);
  const logo = all.find((n) => n.props.nativeID === 'logo');
  assert.match(logo.props.source.uri, /^assets\/logo@2x-[0-9a-f]{8}\.png$/);
  assert.equal(logo.props.style.width, 32, '@2x halves the intrinsic size');
  assert.equal(logo.props.style.height, 16);
  const grad = all.find((n) => n.props.nativeID === 'grad');
  assert.equal(grad.props.style.experimental_backgroundImage, 'linear-gradient(180deg, red, blue)');
});

test('build: minify + sourcemap + dev define', async () => {
  const dir = await fixture();
  const out = path.join(dir, 'min', 'bundle.js');
  await build({ entry: path.join(dir, 'App.js'), outfile: out, minify: true, sourcemap: true, mode: 'development' });
  const code = await readFile(out, 'utf8');
  assert.match(code, /sourceMappingURL=bundle\.js\.map/);
  JSON.parse(await readFile(out + '.map', 'utf8'));
  const { nodes } = runBundle(code);
  await new Promise((res) => setImmediate(res));
  const title = [...nodes.values()].find((n) => n.props.nativeID === 'title');
  assert.match(title.children.map((c) => nodes.get(c).text).join(''), / dev /);
});

test('build errors are reported with file and line', async () => {
  const dir = await fixture();
  await writeFile(path.join(dir, 'Bad.js'), `import { View } from 'react-native';\nexport default () => <View>;\n`);
  await assert.rejects(build({ entry: path.join(dir, 'Bad.js'), outfile: path.join(dir, 'x.js') }), /Bad\.js:2/);
});

test('cli binary: help, build, bad args', async () => {
  const run = promisify(execFile);
  const help = await run(process.execPath, [BIN, '--help']);
  assert.match(help.stdout, /jsui build <entry>/);
  const dir = await fixture();
  const r = await run(process.execPath, [BIN, 'build', path.join(dir, 'App.js'), '-o', path.join(dir, 'o', 'b.js'), '--minify']);
  assert.match(r.stdout, /wrote .*b\.js/);
  await assert.rejects(run(process.execPath, [BIN, 'build']), (e) => e.code === 2);
});

test('dev server: bundle, version token, long-poll, keeps last good bundle on error', async () => {
  const dir = await fixture();
  const logs = [];
  const server = await dev({ entry: path.join(dir, 'App.js'), port: 0, log: (m) => logs.push(m) });
  const base = `http://127.0.0.1:${server.port}`;
  try {
    const r1 = await fetch(base + '/bundle.js');
    const b1 = await r1.text();
    assert.match(b1, /Héllo|H\\u00e9llo/);
    const v1 = await (await fetch(base + '/__jsui/version')).text();
    assert.equal(r1.headers.get('x-jsui-version'), v1);
    assert.ok(v1.length > 0);
    const assetName = [...server.state.assets.keys()].find((k) => k.endsWith('.png'));
    const asset = await fetch(base + '/' + assetName);
    assert.equal(asset.status, 200);
    assert.equal(asset.headers.get('content-type'), 'image/png');
    assert.equal((await fetch(base + '/data.json')).status, 200, 'static files from the entry dir');
    assert.equal((await fetch(base + '/..%2F..%2Fetc%2Fpasswd')).status, 404);
    assert.match(await (await fetch(base + '/')).text(), /jsui dev server/);

    // Long-poll resolves when a rebuild finishes.
    const poll = fetch(base + '/__jsui/version?since=' + v1).then((r) => r.text());
    await new Promise((r) => setTimeout(r, 100));
    await writeFile(path.join(dir, 'data.json'), JSON.stringify({ title: 'Changed' }));
    const v2 = await poll;
    assert.notEqual(v2, v1);
    assert.match(await (await fetch(base + '/bundle.js')).text(), /Changed/);

    // Syntax error: the last good bundle keeps being served, version unchanged.
    await new Promise((r) => setTimeout(r, 300));  // let duplicate fs events settle
    const good = server.state.version;
    await writeFile(path.join(dir, 'App.js'), `import React from 'react';\nconst x = <View>;\n`);
    for (let i = 0; i < 50 && !server.state.error; i++) await new Promise((r) => setTimeout(r, 50));
    assert.match(await (await fetch(base + '/__jsui/error')).text(), /App\.js/);
    assert.equal(await (await fetch(base + '/__jsui/version')).text(), good);
    assert.match(await (await fetch(base + '/bundle.js')).text(), /Changed/);
  } finally {
    await server.close();
  }
});

test('imageSize parses png/gif/svg', () => {
  assert.deepEqual(imageSize(png(7, 9), 'png'), { width: 7, height: 9 });
  const gif = Buffer.from('GIF89a\x05\x00\x03\x00', 'latin1');
  assert.deepEqual(imageSize(gif, 'gif'), { width: 5, height: 3 });
  assert.deepEqual(imageSize(Buffer.from('<svg xmlns="x" viewBox="0 0 24 12">'), 'svg'), { width: 24, height: 12 });
  assert.deepEqual(imageSize(Buffer.from('<svg width="10px" height="20">'), 'svg'), { width: 10, height: 20 });
});
