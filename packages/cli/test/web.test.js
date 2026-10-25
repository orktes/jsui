import { test } from 'node:test';
import assert from 'node:assert/strict';
import { cp, mkdtemp, mkdir, readFile, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { writeWebHost } from '../src/index.js';

const WEB = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..', 'web');

// The repo's host scripts with stand-in WASM modules (the real ones are built
// by scripts/build-wasm.sh), dist-webgl left out unless asked.
async function fakeWebDir(dirs = ['dist', 'dist-gpu']) {
  const dir = await mkdtemp(path.join(tmpdir(), 'jsui-webpkg-'));
  for (const f of ['jsui-web.js', 'jsui-webgpu.js', 'jsui-textures.js', 'jsui-audio.js', 'jsui-worker.js']) {
    await cp(path.join(WEB, f), path.join(dir, f));
  }
  for (const d of dirs) {
    await mkdir(path.join(dir, d));
    await writeFile(path.join(dir, d, 'jsui.js'), `// ${d} loader`);
    await writeFile(path.join(dir, d, 'jsui.wasm'), `${d} wasm`);
  }
  return dir;
}

async function site(webDir, opts = {}) {
  const outDir = await mkdtemp(path.join(tmpdir(), 'jsui-site-'));
  await mkdir(path.join(outDir, 'app'));
  await writeFile(path.join(outDir, 'app/bundle.js'), 'globalThis.app = 1;');
  return { outDir, r: await writeWebHost({ outDir, bundle: 'app/bundle.js', webDir, ...opts }) };
}

test('writeWebHost: every module deployed, versions written into jsui-web.js, imports versioned', async () => {
  const { outDir, r } = await site(await fakeWebDir(['dist', 'dist-webgl', 'dist-gpu']));
  assert.deepEqual(Object.keys(r.modules), ['dist', 'dist-webgl', 'dist-gpu']);
  assert.equal(r.index, 'written');
  for (const d of Object.keys(r.modules)) assert.equal(await readFile(path.join(outDir, d, 'jsui.wasm'), 'utf8'), `${d} wasm`);
  const web = await readFile(path.join(outDir, 'jsui-web.js'), 'utf8');
  assert.ok(web.includes(`const DEPLOYED_MODULES = ${JSON.stringify(r.modules)};`));
  assert.match(web, /from '\.\/jsui-webgpu\.js\?v=[0-9a-f]{16}'/);
  assert.match(web, /import\('\.\/jsui-worker\.js\?v=[0-9a-f]{16}'\)/);
  const html = await readFile(path.join(outDir, 'index.html'), 'utf8');
  assert.match(html, /from '\.\/jsui-web\.js\?v=[0-9a-f]{16}'/);
  assert.match(html, /bundleUrl: '\.\/app\/bundle\.js\?v=[0-9a-f]{16}'/);
});

test('writeWebHost: a second build keeps index.html and updates its versions', async () => {
  const webDir = await fakeWebDir();
  const { outDir } = await site(webDir);
  const first = await readFile(path.join(outDir, 'index.html'), 'utf8');
  await writeFile(path.join(outDir, 'index.html'), first.replace('<title>JSUI</title>', '<title>Mine</title>'));
  await writeFile(path.join(outDir, 'app/bundle.js'), 'globalThis.app = 2;');
  const r = await writeWebHost({ outDir, bundle: 'app/bundle.js', webDir });
  assert.equal(r.index, 'kept');
  const html = await readFile(path.join(outDir, 'index.html'), 'utf8');
  assert.ok(html.includes('<title>Mine</title>'));
  const v = (s) => s.match(/app\/bundle\.js\?v=([0-9a-f]+)/)[1];
  assert.notEqual(v(html), v(first));
});

test('writeWebHost: --html page gets the versioned jsui-web.js import', async () => {
  const page = path.join(await mkdtemp(path.join(tmpdir(), 'jsui-page-')), 'page.html');
  await writeFile(page, `<script type="module">import { createJsui } from "./jsui-web.js";</script>`);
  const { outDir, r } = await site(await fakeWebDir(), { html: page });
  assert.equal(r.index, 'copied');
  assert.match(await readFile(path.join(outDir, 'index.html'), 'utf8'), /from "\.\/jsui-web\.js\?v=[0-9a-f]{16}"/);
});
