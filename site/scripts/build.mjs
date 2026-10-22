#!/usr/bin/env node
// Builds the showcase site into a static directory (GitHub Pages ready):
//
//   node site/scripts/build.mjs [--out site/dist] [--wasm web/dist] [--wasm-webgl web/dist-webgl] [--no-minify]
//
// Layout of the output:
//   index.html, jsui-web.js         page + browser host (web/jsui-web.js)
//   jsui/dist/jsui.{js,wasm}        CPU module (required)
//   jsui/dist-webgl/jsui.{js,wasm}  GPU module (optional; used when present)
//   modules.json                    which modules were deployed
//   app/bundle.js, app/assets/*     the site itself (site/app, built with @jsui/cli)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { build } from '@jsui/cli';

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
const minify = !args.includes('--no-minify');

const has = (dir) => fs.existsSync(path.join(dir, 'jsui.wasm')) && fs.existsSync(path.join(dir, 'jsui.js'));
if (!has(wasm)) {
  console.error(`site: ${wasm}/jsui.{js,wasm} missing (scripts/build-wasm.sh, or download the CI 'jsui-wasm' artifact)`);
  process.exit(1);
}

fs.rmSync(out, { recursive: true, force: true });
fs.mkdirSync(out, { recursive: true });

const copyModule = (from, to) => {
  fs.mkdirSync(to, { recursive: true });
  for (const f of ['jsui.js', 'jsui.wasm']) fs.copyFileSync(path.join(from, f), path.join(to, f));
};
copyModule(wasm, path.join(out, 'jsui/dist'));
const gpu = has(wasmGl);
if (gpu) copyModule(wasmGl, path.join(out, 'jsui/dist-webgl'));
fs.writeFileSync(path.join(out, 'modules.json'), JSON.stringify({ cpu: true, gpu }) + '\n');

fs.copyFileSync(path.join(ROOT, 'web/jsui-web.js'), path.join(out, 'jsui-web.js'));
fs.copyFileSync(path.join(SITE, 'index.html'), path.join(out, 'index.html'));
fs.writeFileSync(path.join(out, '.nojekyll'), '');

const t0 = Date.now();
const r = await build({ entry: path.join(SITE, 'app/index.jsx'), outfile: path.join(out, 'app/bundle.js'), minify });
for (const w of r.warnings) console.warn('site: warning:', w.text);
console.log(`site: bundle ${(r.bytes / 1024).toFixed(1)} KiB, ${r.assets.length} assets in ${Date.now() - t0} ms`);
console.log(`site: wrote ${out} (modules: cpu${gpu ? ' + gpu' : ''})`);
