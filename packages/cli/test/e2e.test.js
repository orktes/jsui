// Builds test/e2e/app.jsx (production and development React) and drives it
// through the real runtime: tests/native/react_e2e_test.cpp sends mouse,
// keyboard, text and gamepad input through the C API and checks the host
// messages the app posts back. Skipped when build/jsui-tests is missing
// (override the path with JSUI_TESTS).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { cp, mkdir, mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { build, dev, findWasmDir } from '../src/index.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..', '..');
const binary = process.env.JSUI_TESTS || path.join(repo, 'build', 'jsui-tests');

for (const mode of ['production', 'development']) {
  test(`native runtime e2e (${mode} React)`, { skip: !existsSync(binary) && `no ${binary}` }, async () => {
    const dir = await mkdtemp(path.join(tmpdir(), 'jsui-e2e-'));
    // Production bundles run as precompiled bytecode when the WebAssembly
    // runtime is around to compile them (jsui build's default).
    const bytecode = mode === 'production' && !!findWasmDir();
    const format = bytecode ? 'combo' : 'source';
    const outfile = path.join(dir, 'bundle.js');
    const scrollfile = path.join(dir, 'scroll.js');
    const interactionsfile = path.join(dir, 'interactions.js');
    const nativeAnimatedFile = path.join(dir, 'native_animated.js');
    await build({ entry: path.join(here, 'e2e', 'app.jsx'), outfile, mode, format });
    await build({ entry: path.join(here, 'e2e', 'scroll.jsx'), outfile: scrollfile, mode, format });
    await build({ entry: path.join(here, 'e2e', 'interactions.jsx'), outfile: interactionsfile, mode, format });
    await build({ entry: path.join(here, 'e2e', 'native_animated.jsx'), outfile: nativeAnimatedFile, mode, format });
    const hostDataFile = path.join(dir, 'host_data.js');
    await build({ entry: path.join(here, 'e2e', 'host_data.jsx'), outfile: hostDataFile, mode, format });
    const imagesFile = path.join(dir, 'images.js');
    await build({ entry: path.join(here, 'e2e', 'images.jsx'), outfile: imagesFile, mode, format });
    const surfacesFile = path.join(dir, 'surfaces.js');
    await build({ entry: path.join(here, 'e2e', 'surfaces.jsx'), outfile: surfacesFile, mode, format });
    const hostBuffersFile = path.join(dir, 'host_buffers.js');
    await build({ entry: path.join(here, 'e2e', 'host_buffers.jsx'), outfile: hostBuffersFile, mode, format });
    const deriveFile = path.join(dir, 'derive.js');
    await build({ entry: path.join(here, 'e2e', 'derive.jsx'), outfile: deriveFile, mode, format });
    const stateStylesFile = path.join(dir, 'state_styles.js');
    await build({ entry: path.join(here, 'e2e', 'state-styles.jsx'), outfile: stateStylesFile, mode, format });
    const groupVisibilityFile = path.join(dir, 'group_visibility.js');
    await build({ entry: path.join(here, 'e2e', 'group-visibility.jsx'), outfile: groupVisibilityFile, mode, format });
    const tests = [
      'react_e2e',
      'react_interactions_e2e',
      'react_native_animated_e2e',
      'react_host_data_e2e',
      'react_host_data_e2e_js_thread_mode',
      'react_images_e2e',
      'react_surfaces_e2e',
      'react_host_buffers_e2e',
      'react_derive_e2e',
      'react_state_styles_e2e',
      'react_group_visibility_e2e',
    ];
    const { stdout, stderr } = await promisify(execFile)(binary, tests, {
      env: Object.assign({}, process.env, {
        JSUI_REACT_E2E_BUNDLE: outfile,
        JSUI_REACT_E2E_SCROLL_BUNDLE: scrollfile,
        JSUI_REACT_INTERACTIONS_BUNDLE: interactionsfile,
        JSUI_REACT_NATIVE_ANIMATED_BUNDLE: nativeAnimatedFile,
        JSUI_REACT_HOST_DATA_BUNDLE: hostDataFile,
        JSUI_REACT_IMAGES_BUNDLE: imagesFile,
        JSUI_REACT_SURFACES_BUNDLE: surfacesFile,
        JSUI_REACT_HOST_BUFFERS_BUNDLE: hostBuffersFile,
        JSUI_REACT_DERIVE_BUNDLE: deriveFile,
        JSUI_REACT_STATE_STYLES_BUNDLE: stateStylesFile,
        JSUI_REACT_GROUP_VISIBILITY_BUNDLE: groupVisibilityFile,
      }),
      cwd: repo,
    }).catch((e) => {
      throw new Error(`jsui-tests failed:\n${e.stdout}\n${e.stderr}`);
    });
    assert.match(stdout, /PASS react_e2e$/m);
    assert.match(stdout, /PASS react_e2e_scroll/);
    assert.match(stdout, /PASS react_interactions_e2e/);
    assert.match(stdout, /PASS react_native_animated_e2e/);
    assert.match(stdout, /PASS react_host_data_e2e$/m);
    assert.match(stdout, /PASS react_host_data_e2e_js_thread_mode/);
    assert.match(stdout, /PASS react_images_e2e/);
    assert.match(stdout, /PASS react_surfaces_e2e$/m);
    assert.match(stdout, /PASS react_surfaces_e2e_host_picks_main/);
    assert.match(stdout, /PASS react_surfaces_e2e_error_isolation/);
    assert.match(stdout, /PASS react_host_buffers_e2e/);
    assert.match(stdout, /PASS react_derive_e2e/);
    assert.match(stdout, /PASS react_state_styles_e2e/);
    assert.match(stdout, /PASS react_group_visibility_e2e/);
    if (process.env.JSUI_E2E_VERBOSE) console.log(stdout);
    assert.doesNotMatch(stdout, /skipped/);
    // The WebAssembly runtime compiled the bytecode: a native runtime built
    // from the same QuickJS sources must run it, not the fallback source.
    if (bytecode) assert.doesNotMatch(stderr, /running the bundle's source instead/, 'web/dist and the native build use different QuickJS sources: rebuild one');
  });

  // Virtualized FlatList (10,000 rows): windowing, focus navigation, wheel,
  // scrollToIndex / scrollToEnd, checked from rendered pixels; prints timings.
  test(`native runtime list e2e (${mode} React)`, { skip: !existsSync(binary) && `no ${binary}` }, async () => {
    const dir = await mkdtemp(path.join(tmpdir(), 'jsui-e2e-list-'));
    const outfile = path.join(dir, 'bundle.js');
    const bytecode = mode === 'production' && !!findWasmDir();
    const format = bytecode ? 'split' : 'source';
    await build({ entry: path.join(here, 'e2e', 'list.jsx'), outfile, mode, format });
    const { stdout, stderr } = await promisify(execFile)(binary, ['react_list_e2e'], {
      env: Object.assign({}, process.env, { JSUI_REACT_LIST_BUNDLE: outfile }),
      cwd: repo,
    }).catch((e) => {
      throw new Error(`jsui-tests failed:\n${e.stdout}\n${e.stderr}`);
    });
    if (process.env.JSUI_E2E_VERBOSE) console.log(stdout);
    assert.match(stdout, /PASS react_list_e2e/);
    assert.doesNotMatch(stdout, /skipped/);
    if (bytecode) assert.doesNotMatch(stderr, /running the bundle's source instead/);
  });
}

// Hot updates in the real runtime: `jsui dev` serves test/e2e/hmr (copied to a
// temp dir) and tests/native/react_hmr_e2e_test.cpp loads it in dev mode,
// then copies steps/<n>/* over the app and checks what renders.
test('native runtime hot updates (jsui dev)', { skip: !existsSync(binary) && `no ${binary}` }, async () => {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-e2e-hmr-'));
  await cp(path.join(here, 'e2e', 'hmr'), dir, { recursive: true });
  const app = await readFile(path.join(dir, 'App.jsx'), 'utf8');
  const index = await readFile(path.join(dir, 'index.jsx'), 'utf8');
  const steps = [
    { 'App.jsx': app.replace("'v1 '", "'v2 '") },
    { 'label.js': "export const label = 'b';\n" },
    { 'App.jsx': app.replace("const text = 'v1 '", 'const text = <View') },
    { 'App.jsx': app.replace("'v1 '", "'v3 '") },
    { 'App.jsx': app.replace("const text = 'v1 '", "if (n >= 0) throw new Error('render boom');\n  const text = 'v4 '") },
    { 'App.jsx': app.replace("'v1 '", "'v5 '") },
    { 'index.jsx': index + '// edited\n' },
  ];
  for (let i = 0; i < steps.length; i++) {
    const d = path.join(dir, 'steps', String(i + 1));
    await mkdir(d, { recursive: true });
    for (const [f, c] of Object.entries(steps[i])) await writeFile(path.join(d, f), c);
  }
  // The steps directory is not imported, so the watcher ignores it.
  const server = await dev({ entry: path.join(dir, 'index.jsx'), port: 0, log: () => {} });
  try {
    const { stdout } = await promisify(execFile)(binary, ['react_hmr_e2e'], {
      env: Object.assign({}, process.env, {
        JSUI_REACT_HMR_URL: `http://127.0.0.1:${server.port}/bundle.js`,
        JSUI_REACT_HMR_DIR: dir,
      }),
      cwd: repo,
    }).catch((e) => {
      throw new Error(`jsui-tests failed:\n${e.stdout}\n${e.stderr}`);
    });
    if (process.env.JSUI_E2E_VERBOSE) console.log(stdout);
    assert.match(stdout, /PASS react_hmr_e2e/);
    assert.doesNotMatch(stdout, /skipped/);
  } finally {
    await server.close();
  }
});
