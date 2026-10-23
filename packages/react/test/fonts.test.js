// Fonts / useFonts / the expo-font shim against the mock bridge: source
// forms, weight / style mapping, face arrays, dedupe, unload, fallbacks and
// the useFonts states. Font registration itself is native.
import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h } from 'react';
import { Fonts, useFonts, render, Text } from '../src/index.js';
import * as ExpoFont from '../src/shims/expo-font.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const fontCalls = (name = 'loadFont') => mock.calls.filter((c) => c[0] === name).map((c) => c.slice(1));

function captureWarnings(fn) {
  const warn = console.warn;
  const out = [];
  console.warn = (...a) => out.push(a.join(' '));
  try {
    return [fn(), out];
  } finally {
    console.warn = warn;
  }
}

test('source forms: URL, {uri}, {default}, nested face sources, bytes', async () => {
  mock.reset();
  assert.equal(await Fonts.load('A', 'assets/a.ttf'), true);
  await Fonts.load('B', { uri: 'https://example.com/b.otf' });
  await Fonts.load('C', { default: 'assets/c.woff2' });
  await Fonts.load('D', { default: { uri: 'assets/d.ttf' } });
  assert.deepEqual(fontCalls(), [
    ['A', 'assets/a.ttf', 0, -1],
    ['B', 'https://example.com/b.otf', 0, -1],
    ['C', 'assets/c.woff2', 0, -1],
    ['D', 'assets/d.ttf', 0, -1],
  ]);
  // Bytes register synchronously through loadFontData.
  const bytes = new Uint8Array([0, 1, 0, 0, 9, 9]);
  const p = Fonts.load('Bytes', bytes, { weight: 'bold' });
  assert.equal(Fonts.isLoaded('Bytes'), true);
  assert.equal(await p, true);
  await Fonts.load('Buf', bytes.buffer);
  assert.deepEqual(fontCalls('loadFontData'), [
    ['Bytes', 6, 700, -1],
    ['Buf', 6, 0, -1],
  ]);
  // Bad bytes reject (and are not cached).
  await assert.rejects(Fonts.load('Bad', new Uint8Array(2)), /not a font/);
  assert.equal(Fonts.isLoaded('Bad'), false);
  // Numeric asset ids (Metro) are unsupported: warn and reject.
  const [r, warnings] = captureWarnings(() => Fonts.load('Num', 42));
  await assert.rejects(r, /unsupported asset id 42/);
  assert.match(warnings.join('\n'), /numeric asset ids are not supported/);
  await assert.rejects(Fonts.load('None', null), /no font source/);
});

test('weight and style mapping', async () => {
  mock.reset();
  const cases = [
    [{}, 0, -1],
    [{ weight: 700 }, 700, -1],
    [{ weight: '300' }, 300, -1],
    [{ weight: 'bold' }, 700, -1],
    [{ weight: 'normal' }, 400, -1],
    [{ weight: 'Semi Bold' }, 600, -1],
    [{ weight: 'black', style: 'italic' }, 900, 1],
    [{ style: 'normal' }, 0, 0],
    [{ style: 'oblique' }, 0, 1],
    [{ italic: true }, 0, 1],
    [{ weight: 1200 }, 1000, -1],
  ];
  for (const [i, [opts]] of cases.entries()) await Fonts.load('W', 'assets/w' + i + '.ttf', opts);
  assert.deepEqual(
    fontCalls().map((c) => [c[2], c[3]]),
    cases.map((c) => [c[1], c[2]]),
  );
});

test('identical loads dedupe; failed loads can be retried', async () => {
  mock.reset();
  const a = Fonts.load('Dup', 'assets/dup.ttf', { weight: 400 });
  const b = Fonts.load('Dup', 'assets/dup.ttf', { weight: 'normal' });
  assert.equal(a, b);
  assert.equal(Fonts.isLoading('Dup'), true);
  await a;
  assert.equal(Fonts.isLoading('Dup'), false);
  await Fonts.load('Dup', 'assets/dup.ttf', { weight: 400 });
  await Fonts.load('Dup', 'assets/dup-bold.ttf', { weight: 700 }); // another face
  assert.equal(fontCalls().length, 2);
  await assert.rejects(Fonts.load('Gone', 'assets/missing.ttf'), /404/);
  await assert.rejects(Fonts.load('Gone', 'assets/missing.ttf'), /404/);
  assert.equal(fontCalls().length, 4);
  assert.equal(Fonts.isLoaded('Gone'), false);
});

test('loadAsync: name + source, maps, face descriptors and face arrays', async () => {
  mock.reset();
  assert.equal(await Fonts.loadAsync('One', 'assets/one.ttf'), undefined);
  await Fonts.loadAsync({
    Two: 'assets/two.ttf',
    Three: { uri: 'assets/three.ttf', weight: '600', style: 'italic', display: 'swap' },
    Game: [
      { source: 'assets/game-regular.ttf', weight: 400 },
      { source: { default: 'assets/game-bold.ttf' }, weight: 'bold' },
      { uri: 'assets/game-italic.ttf', weight: 400, style: 'italic' },
    ],
  });
  assert.deepEqual(fontCalls(), [
    ['One', 'assets/one.ttf', 0, -1],
    ['Two', 'assets/two.ttf', 0, -1],
    ['Three', 'assets/three.ttf', 600, 1],
    ['Game', 'assets/game-regular.ttf', 400, -1],
    ['Game', 'assets/game-bold.ttf', 700, -1],
    ['Game', 'assets/game-italic.ttf', 400, 1],
  ]);
  for (const f of ['One', 'Two', 'Three', 'Game']) assert.equal(Fonts.isLoaded(f), true);
  assert.ok(Fonts.getLoadedFonts().includes('Game'));
  assert.ok(Fonts.getLoadedFonts().includes('Inter'));
  await assert.rejects(Fonts.loadAsync({ Ok: 'assets/ok.ttf', Nope: 'assets/missing.ttf' }), /404/);
});

test('unloadAsync / unloadAllAsync only touch fonts loaded through Fonts', async () => {
  await Fonts.loadAsync({ U1: 'assets/u1.ttf', U2: 'assets/u2.ttf', U3: 'assets/u3.ttf' });
  mock.reset();
  await Fonts.unloadAsync('U1');
  await Fonts.unloadAsync({ U2: 'assets/u2.ttf' });
  await Fonts.unloadAsync('Inter'); // not ours: left alone
  assert.deepEqual(fontCalls('unloadFont'), [['U1'], ['U2']]);
  assert.equal(Fonts.isLoaded('U1'), false);
  assert.equal(Fonts.isLoaded('U3'), true);
  // Reloading after unload goes to native again.
  await Fonts.load('U1', 'assets/u1.ttf');
  assert.equal(fontCalls().length, 1);
  mock.reset();
  await Fonts.unloadAllAsync();
  const unloaded = fontCalls('unloadFont').map((c) => c[0]);
  assert.ok(unloaded.includes('U1') && unloaded.includes('U3') && unloaded.includes('Game'));
  assert.ok(!unloaded.includes('Inter'));
  assert.deepEqual(Fonts.getLoadedFonts(), ['Inter']);
});

test('setFallbacks / setDefaultFamily', () => {
  mock.reset();
  Fonts.setFallbacks(['Noto Color Emoji', 'Noto Sans JP']);
  Fonts.setFallbacks("'Noto Sans JP', Noto Sans Arabic");
  Fonts.setFallbacks([]);
  Fonts.setDefaultFamily('Cinzel');
  assert.deepEqual(fontCalls('setFontFallbacks'), [[['Noto Color Emoji', 'Noto Sans JP']], [['Noto Sans JP', 'Noto Sans Arabic']], [[]]]);
  assert.deepEqual(fontCalls('setDefaultFontFamily'), [['Cinzel']]);
});

test('missing native bindings degrade with a warning', async () => {
  const n = globalThis.__jsui_native;
  const saved = { loadFont: n.loadFont, loadFontData: n.loadFontData, getFontFamilies: n.getFontFamilies, setFontFallbacks: n.setFontFallbacks };
  for (const k of Object.keys(saved)) delete n[k];
  try {
    const [p, w1] = captureWarnings(() => Fonts.load('NoNative', 'assets/nn.ttf'));
    assert.equal(await p, false);
    const [q, w2] = captureWarnings(() => Fonts.load('NoNative', new Uint8Array(8)));
    assert.equal(await q, false);
    const [, w3] = captureWarnings(() => Fonts.setFallbacks(['X']));
    assert.match(w1.join(), /loadFont is not available/);
    assert.match(w2.join(), /loadFontData is not available/);
    assert.match(w3.join(), /setFontFallbacks is not available/);
    assert.equal(Fonts.isLoaded('NoNative'), false);
    assert.ok(!Fonts.getLoadedFonts().includes('NoNative'));
    // useFonts still resolves so the app renders with the default font.
    let state;
    render(h(() => ((state = useFonts({ NoNative: 'assets/nn2.ttf' })), null)));
    await wait();
    await wait();
    assert.deepEqual(state, [true, null]);
    render(null);
    await wait();
  } finally {
    Object.assign(n, saved);
  }
});

test('useFonts: [false, null] -> [true, null]; errors; already loaded = no flash', async () => {
  const states = [];
  function App({ map }) {
    const [loaded, error] = useFonts(map);
    states.push([loaded, error && error.message]);
    return loaded ? h(Text, { nativeID: 'title', style: { fontFamily: 'Hook' } }, 'Ready') : null;
  }
  mock.holdFonts = true;
  const map = { Hook: [{ source: 'assets/hook.ttf', weight: 400 }, { source: 'assets/hook-b.ttf', weight: 700 }] };
  render(h(App, { map }));
  await wait();
  assert.deepEqual(states.at(-1), [false, null]);
  assert.equal(Fonts.isLoading('Hook'), true);
  mock.holdFonts = false;
  mock.releaseFonts();
  await wait();
  assert.deepEqual(states.at(-1), [true, null]);
  assert.equal(mock.text(mock.find((n) => n.props.nativeID === 'title').id), 'Ready');
  render(null);
  await wait();

  // Mounted again with the same (now loaded) fonts: loaded on the first render.
  states.length = 0;
  render(h(App, { map: { Hook: [{ source: 'assets/hook.ttf', weight: 400 }, { source: 'assets/hook-b.ttf', weight: 700 }] } }));
  await wait();
  assert.deepEqual(states[0], [true, null]);
  assert.ok(states.every((s) => s[0] === true));

  // A failing font: [false, Error].
  render(h(App, { map: { Broken: 'assets/missing.ttf' } }));
  await wait();
  assert.equal(states.at(-1)[0], false);
  assert.match(states.at(-1)[1], /404/);
  render(null);
  await wait();
});

test('expo-font shim: the expo API and an @expo-google-fonts style package', async () => {
  mock.reset();
  assert.equal(ExpoFont.FontDisplay.SWAP, 'swap');
  assert.equal(ExpoFont.useFonts, useFonts);
  // @expo-google-fonts/<family>: export const X_400Regular = require('./400Regular/X.ttf')
  const Lato_400Regular = 'assets/Lato_400Regular-0123abcd.ttf';
  const Lato_700Bold = { default: 'assets/Lato_700Bold-0123abcd.ttf' }; // ESM interop
  await ExpoFont.loadAsync({ Lato_400Regular, Lato_700Bold });
  assert.equal(ExpoFont.isLoaded('Lato_400Regular'), true);
  assert.equal(ExpoFont.isLoading('Lato_700Bold'), false);
  assert.ok(ExpoFont.getLoadedFonts().includes('Lato_700Bold'));
  assert.deepEqual(fontCalls(), [
    ['Lato_400Regular', Lato_400Regular, 0, -1],
    ['Lato_700Bold', Lato_700Bold.default, 0, -1],
  ]);
  await ExpoFont.unloadAsync('Lato_400Regular');
  assert.equal(ExpoFont.isLoaded('Lato_400Regular'), false);
  await ExpoFont.unloadAllAsync();
  assert.equal(ExpoFont.isLoaded('Lato_700Bold'), false);
});
