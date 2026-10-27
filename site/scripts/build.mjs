#!/usr/bin/env node
// Builds the showcase site into a static directory (GitHub Pages ready):
//
//   node site/scripts/build.mjs [--out site/dist] [--wasm web/dist] [--wasm-webgl web/dist-webgl]
//                               [--wasm-gpu web/dist-gpu] [--no-minify]
//
// Layout of the output:
//   index.html, jsui-web.js         page + browser host (web/jsui-web.js,
//   jsui-webgpu.js, jsui-textures.js, and its WebGPU renderer, host textures,
//   jsui-worker.js, jsui-audio.js   worker mode and Web Audio; jsui-web.js is also the worker's entry)
//   jsui/dist/jsui.{js,wasm}        CPU module (required)
//   jsui/dist-webgl/jsui.{js,wasm}  WebGL module (optional; used when present)
//   jsui/dist-gpu/jsui.{js,wasm}    WebGPU + WebGL + CPU module (optional; preferred when
//                                   present and the browser has WebGPU)
//   modules.json                    which modules were deployed, and their versions
//   app/bundle.js, app/assets/*     the site itself (site/app, built with @orktes/jsui-cli)
//   llms.txt, og.png,               site/public, copied as is
//   pipelines-{bgra8,rgba8}unorm.bin
//   three/{index.html,main.js,ui.js} the Three.js example (site/three; Three.js bundled into main.js)
//
// Cache consistency: GitHub Pages serves everything with max-age=600, so a
// browser may hold some files of the previous deploy and fetch others fresh
// (a new index.html with an old jsui-web.js, a jsui.js with the jsui.wasm of
// another build: "Importing a module script failed"). Every URL the page
// loads besides index.html itself is therefore versioned with a content
// hash, ?v=<hash>: jsui-web.js (imported by index.html, and the worker's
// script in worker mode), jsui-webgpu.js, jsui-textures.js, jsui-components.js, jsui-worker.js and
// jsui-audio.js (imported by jsui-web.js, rewritten here), each module's jsui.js and its
// jsui.wasm (one hash over both; the host passes the module URL's query on to
// the wasm, see locateIn() in web/jsui-web.js) and app/bundle.js. index.html
// gets the module versions inline (no modules.json fetch to go stale).
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { build } from '@orktes/jsui-cli';

const SITE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ROOT = path.resolve(SITE, '..');

const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : def;
};
const out = path.resolve(opt('--out', path.join(SITE, 'dist')));
const wasm = path.resolve(opt('--wasm', path.join(ROOT, 'web/dist')));
const wasmGl = path.resolve(opt('--wasm-webgl', path.join(ROOT, 'web/dist-webgl')));
const wasmGpu = path.resolve(opt('--wasm-gpu', path.join(ROOT, 'web/dist-gpu')));
const minify = !args.includes('--no-minify');

const has = (dir) => fs.existsSync(path.join(dir, 'jsui.wasm')) && fs.existsSync(path.join(dir, 'jsui.js'));
if (!has(wasm)) {
  console.error(`site: ${wasm}/jsui.{js,wasm} missing (scripts/build-wasm.sh, or download the CI 'jsui-wasm' artifact)`);
  process.exit(1);
}

fs.rmSync(out, { recursive: true, force: true });
fs.mkdirSync(out, { recursive: true });

const hash = (...bufs) => {
  const h = crypto.createHash('sha256');
  for (const b of bufs) h.update(b);
  return h.digest('hex').slice(0, 16);
};
// Replaces every `from` (there must be one) in `text`.
const replaceAll = (text, from, to, file) => {
  if (!text.includes(from)) {
    console.error(`site: ${file} has no ${JSON.stringify(from)} to version`);
    process.exit(1);
  }
  return text.split(from).join(to);
};

// Modules: one version per directory, a hash over its jsui.js and jsui.wasm.
const versions = {};
const copyModule = (from, dir) => {
  const to = path.join(out, 'jsui', dir);
  fs.mkdirSync(to, { recursive: true });
  const files = ['jsui.js', 'jsui.wasm'];
  const data = files.map((f) => fs.readFileSync(path.join(from, f)));
  files.forEach((f, i) => fs.writeFileSync(path.join(to, f), data[i]));
  versions[dir] = hash(...data);
};
copyModule(wasm, 'dist');
const gpu = has(wasmGl);
if (gpu) copyModule(wasmGl, 'dist-webgl');
const webgpu = has(wasmGpu);
if (webgpu) copyModule(wasmGpu, 'dist-gpu');
const modules = { cpu: true, gpu, webgpu, versions };
fs.writeFileSync(path.join(out, 'modules.json'), JSON.stringify(modules) + '\n');

const t0 = Date.now();
const r = await build({ entry: path.join(SITE, 'app/index.jsx'), outfile: path.join(out, 'app/bundle.js'), minify });
for (const w of r.warnings) console.warn('site: warning:', w.text);

// The host scripts, each import pointing at the version deployed with it.
const webgpuJs = fs.readFileSync(path.join(ROOT, 'web/jsui-webgpu.js'));
fs.writeFileSync(path.join(out, 'jsui-webgpu.js'), webgpuJs);
const texturesJs = fs.readFileSync(path.join(ROOT, 'web/jsui-textures.js'));
fs.writeFileSync(path.join(out, 'jsui-textures.js'), texturesJs);
const componentsJs = fs.readFileSync(path.join(ROOT, 'web/jsui-components.js'));
fs.writeFileSync(path.join(out, 'jsui-components.js'), componentsJs);
const audioJs = fs.readFileSync(path.join(ROOT, 'web/jsui-audio.js'));
fs.writeFileSync(path.join(out, 'jsui-audio.js'), audioJs);
const workerJs = fs.readFileSync(path.join(ROOT, 'web/jsui-worker.js'));
fs.writeFileSync(path.join(out, 'jsui-worker.js'), workerJs);
let webJs = fs.readFileSync(path.join(ROOT, 'web/jsui-web.js'), 'utf8');
webJs = replaceAll(webJs, "from './jsui-webgpu.js'", `from './jsui-webgpu.js?v=${hash(webgpuJs)}'`, 'web/jsui-web.js');
webJs = replaceAll(webJs, "from './jsui-textures.js'", `from './jsui-textures.js?v=${hash(texturesJs)}'`, 'web/jsui-web.js');
webJs = replaceAll(webJs, "from './jsui-components.js'", `from './jsui-components.js?v=${hash(componentsJs)}'`, 'web/jsui-web.js');
webJs = replaceAll(webJs, "from './jsui-audio.js'", `from './jsui-audio.js?v=${hash(audioJs)}'`, 'web/jsui-web.js');
webJs = replaceAll(webJs, "import('./jsui-worker.js')", `import('./jsui-worker.js?v=${hash(workerJs)}')`, 'web/jsui-web.js');
fs.writeFileSync(path.join(out, 'jsui-web.js'), webJs);
let html = fs.readFileSync(path.join(SITE, 'index.html'), 'utf8');
html = replaceAll(html, "from './jsui-web.js'", `from './jsui-web.js?v=${hash(webJs)}'`, 'site/index.html');
html = replaceAll(html, "'./app/bundle.js'",
  `'./app/bundle.js?v=${hash(fs.readFileSync(path.join(out, 'app/bundle.js')))}'`, 'site/index.html');
html = replaceAll(html, "components: './components.js'",
  `components: './components.js?v=${hash(fs.readFileSync(path.join(SITE, 'public/components.js')))}'`, 'site/index.html');
html = replaceAll(html, 'const modules = null;  // set by site/scripts/build.mjs',
  `const modules = ${JSON.stringify(modules)};`, 'site/index.html');
fs.writeFileSync(path.join(out, 'index.html'), html);
fs.writeFileSync(path.join(out, '.nojekyll'), '');

// three/: the Three.js example (site/three: a Three.js WebGPU scene and JSUI
// on one GPUDevice). Its page script is bundled with Three.js (tree-shaken);
// jsui-web.js is the one deployed above.
{
  const dir = path.join(out, 'three');
  fs.mkdirSync(dir, { recursive: true });
  const ui = await build({ entry: path.join(SITE, 'three/ui.jsx'), outfile: path.join(dir, 'ui.js'), minify });
  for (const w of ui.warnings) console.warn('site: three: warning:', w.text);
  const esbuild = await import('esbuild');
  const page = await esbuild.build({
    entryPoints: [path.join(SITE, 'three/main.js')], bundle: true, format: 'esm', minify, write: false,
    target: 'es2022', external: ['../jsui-web.js'], legalComments: 'eof', logLevel: 'warning',
  });
  let mainJs = Buffer.from(page.outputFiles[0].contents).toString('utf8');
  mainJs = replaceAll(mainJs, '"../jsui-web.js"', `"../jsui-web.js?v=${hash(webJs)}"`, 'three/main.js');
  mainJs = replaceAll(mainJs, '"./ui.js"', `"./ui.js?v=${hash(fs.readFileSync(path.join(dir, 'ui.js')))}"`, 'three/main.js');
  fs.writeFileSync(path.join(dir, 'main.js'), mainJs);
  let threeHtml = fs.readFileSync(path.join(SITE, 'three/index.html'), 'utf8');
  threeHtml = replaceAll(threeHtml, 'src="./main.js"', `src="./main.js?v=${hash(mainJs)}"`, 'site/three/index.html');
  threeHtml = replaceAll(threeHtml, 'window.jsuiModules = null;  // set by site/scripts/build.mjs',
    `window.jsuiModules = ${JSON.stringify(modules)};`, 'site/three/index.html');
  fs.writeFileSync(path.join(dir, 'index.html'), threeHtml);
  console.log(`site: three/ page ${(mainJs.length / 1024).toFixed(1)} KiB, UI bundle ${(ui.bytes / 1024).toFixed(1)} KiB`);
}
// Static files served as they are: llms.txt, the social preview image (og.png),
// the GPU pipeline lists (site/scripts/record-pipelines.mjs).
fs.cpSync(path.join(SITE, 'public'), out, { recursive: true });
console.log(`site: bundle ${(r.bytes / 1024).toFixed(1)} KiB, ${r.assets.length} assets in ${Date.now() - t0} ms`);
console.log(`site: wrote ${out} (modules: cpu${gpu ? ' + webgl' : ''}${webgpu ? ' + webgpu' : ''})`);
