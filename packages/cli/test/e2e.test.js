// Builds test/e2e/app.jsx (production and development React) and drives it
// through the real runtime: tests/native/react_e2e_test.cpp sends mouse,
// keyboard, text and gamepad input through the C API and checks the host
// messages the app posts back. Skipped when build/jsui-tests is missing
// (override the path with JSUI_TESTS).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { mkdtemp } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { build } from '../src/index.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..', '..');
const binary = process.env.JSUI_TESTS || path.join(repo, 'build', 'jsui-tests');

for (const mode of ['production', 'development']) {
  test(`native runtime e2e (${mode} React)`, { skip: !existsSync(binary) && `no ${binary}` }, async () => {
    const dir = await mkdtemp(path.join(tmpdir(), 'jsui-e2e-'));
    const outfile = path.join(dir, 'bundle.js');
    const scrollfile = path.join(dir, 'scroll.js');
    const interactionsfile = path.join(dir, 'interactions.js');
    const nativeAnimatedFile = path.join(dir, 'native_animated.js');
    await build({ entry: path.join(here, 'e2e', 'app.jsx'), outfile, mode });
    await build({ entry: path.join(here, 'e2e', 'scroll.jsx'), outfile: scrollfile, mode });
    await build({ entry: path.join(here, 'e2e', 'interactions.jsx'), outfile: interactionsfile, mode });
    await build({ entry: path.join(here, 'e2e', 'native_animated.jsx'), outfile: nativeAnimatedFile, mode });
    const hostDataFile = path.join(dir, 'host_data.js');
    await build({ entry: path.join(here, 'e2e', 'host_data.jsx'), outfile: hostDataFile, mode });
    const imagesFile = path.join(dir, 'images.js');
    await build({ entry: path.join(here, 'e2e', 'images.jsx'), outfile: imagesFile, mode });
    const surfacesFile = path.join(dir, 'surfaces.js');
    await build({ entry: path.join(here, 'e2e', 'surfaces.jsx'), outfile: surfacesFile, mode });
    const hostBuffersFile = path.join(dir, 'host_buffers.js');
    await build({ entry: path.join(here, 'e2e', 'host_buffers.jsx'), outfile: hostBuffersFile, mode });
    const tests = [
      'react_e2e',
      'react_interactions_e2e',
      'react_native_animated_e2e',
      'react_host_data_e2e',
      'react_images_e2e',
      'react_surfaces_e2e',
      'react_host_buffers_e2e',
    ];
    const { stdout } = await promisify(execFile)(binary, tests, {
      env: Object.assign({}, process.env, {
        JSUI_REACT_E2E_BUNDLE: outfile,
        JSUI_REACT_E2E_SCROLL_BUNDLE: scrollfile,
        JSUI_REACT_INTERACTIONS_BUNDLE: interactionsfile,
        JSUI_REACT_NATIVE_ANIMATED_BUNDLE: nativeAnimatedFile,
        JSUI_REACT_HOST_DATA_BUNDLE: hostDataFile,
        JSUI_REACT_IMAGES_BUNDLE: imagesFile,
        JSUI_REACT_SURFACES_BUNDLE: surfacesFile,
        JSUI_REACT_HOST_BUFFERS_BUNDLE: hostBuffersFile,
      }),
      cwd: repo,
    }).catch((e) => {
      throw new Error(`jsui-tests failed:\n${e.stdout}\n${e.stderr}`);
    });
    assert.match(stdout, /PASS react_e2e$/m);
    assert.match(stdout, /PASS react_e2e_scroll/);
    assert.match(stdout, /PASS react_interactions_e2e/);
    assert.match(stdout, /PASS react_native_animated_e2e/);
    assert.match(stdout, /PASS react_host_data_e2e/);
    assert.match(stdout, /PASS react_images_e2e/);
    assert.match(stdout, /PASS react_surfaces_e2e$/m);
    assert.match(stdout, /PASS react_surfaces_e2e_host_picks_main/);
    assert.match(stdout, /PASS react_surfaces_e2e_error_isolation/);
    assert.match(stdout, /PASS react_host_buffers_e2e/);
    if (process.env.JSUI_E2E_VERBOSE) console.log(stdout);
    assert.doesNotMatch(stdout, /skipped/);
  });

  // Virtualized FlatList (10,000 rows): windowing, focus navigation, wheel,
  // scrollToIndex / scrollToEnd, checked from rendered pixels; prints timings.
  test(`native runtime list e2e (${mode} React)`, { skip: !existsSync(binary) && `no ${binary}` }, async () => {
    const dir = await mkdtemp(path.join(tmpdir(), 'jsui-e2e-list-'));
    const outfile = path.join(dir, 'bundle.js');
    await build({ entry: path.join(here, 'e2e', 'list.jsx'), outfile, mode });
    const { stdout } = await promisify(execFile)(binary, ['react_list_e2e'], {
      env: Object.assign({}, process.env, { JSUI_REACT_LIST_BUNDLE: outfile }),
      cwd: repo,
    }).catch((e) => {
      throw new Error(`jsui-tests failed:\n${e.stdout}\n${e.stderr}`);
    });
    if (process.env.JSUI_E2E_VERBOSE) console.log(stdout);
    assert.match(stdout, /PASS react_list_e2e/);
    assert.doesNotMatch(stdout, /skipped/);
  });
}
