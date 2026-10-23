#!/usr/bin/env node
// Builds the showcase site into a static directory (GitHub Pages ready):
//
//   node site/scripts/build.mjs [--out site/dist] [--wasm web/dist] [--wasm-webgl web/dist-webgl]
//                               [--wasm-gpu web/dist-gpu] [--no-minify]
//
// Layout of the output:
//   index.html, jsui-web.js         page + browser host (web/jsui-web.js,
//   jsui-webgpu.js, jsui-textures.js and its WebGPU renderer and host textures)
//   jsui/dist/jsui.{js,wasm}        CPU module (required)
//   jsui/dist-webgl/jsui.{js,wasm}  WebGL module (optional; used when present)
//   jsui/dist-gpu/jsui.{js,wasm}    WebGPU + WebGL + CPU module (optional; preferred when
//                                   present and the browser has WebGPU)
//   modules.json                    which modules were deployed, and their versions
//   app/bundle.js, app/assets/*     the site itself (site/app, built with @orktes/jsui-cli)
//
// Cache consistency: GitHub Pages serves everything with max-age=600, so a
// browser may hold some files of the previous deploy and fetch others fresh
// (a new index.html with an old jsui-web.js, a jsui.js with the jsui.wasm of
// another build: "Importing a module script failed"). Every URL the page
// loads besides index.html itself is therefore versioned with a content
// hash, ?v=<hash>: jsui-web.js (imported by index.html), jsui-webgpu.js and
// jsui-textures.js (imported by jsui-web.js, rewritten here), each module's jsui.js and its
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
let webJs = fs.readFileSync(path.join(ROOT, 'web/jsui-web.js'), 'utf8');
webJs = replaceAll(webJs, "from './jsui-webgpu.js'", `from './jsui-webgpu.js?v=${hash(webgpuJs)}'`, 'web/jsui-web.js');
webJs = replaceAll(webJs, "from './jsui-textures.js'", `from './jsui-textures.js?v=${hash(texturesJs)}'`, 'web/jsui-web.js');
fs.writeFileSync(path.join(out, 'jsui-web.js'), webJs);
let html = fs.readFileSync(path.join(SITE, 'index.html'), 'utf8');
html = replaceAll(html, "from './jsui-web.js'", `from './jsui-web.js?v=${hash(webJs)}'`, 'site/index.html');
html = replaceAll(html, "'./app/bundle.js'",
  `'./app/bundle.js?v=${hash(fs.readFileSync(path.join(out, 'app/bundle.js')))}'`, 'site/index.html');
html = replaceAll(html, 'const modules = null;  // set by site/scripts/build.mjs',
  `const modules = ${JSON.stringify(modules)};`, 'site/index.html');
fs.writeFileSync(path.join(out, 'index.html'), html);
fs.writeFileSync(path.join(out, '.nojekyll'), '');
console.log(`site: bundle ${(r.bytes / 1024).toFixed(1)} KiB, ${r.assets.length} assets in ${Date.now() - t0} ms`);
console.log(`site: wrote ${out} (modules: cpu${gpu ? ' + webgl' : ''}${webgpu ? ' + webgpu' : ''})`);
