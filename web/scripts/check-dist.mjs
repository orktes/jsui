// prepack: refuse to pack @orktes/jsui-web without all three WASM modules. They are
// build outputs (scripts/build-wasm.sh, or the CI artifacts jsui-wasm,
// jsui-wasm-webgl, jsui-wasm-gpu), not checked in, and npm would otherwise
// silently publish a package whose default module is missing.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const WEB = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const missing = [];
for (const dir of ['dist', 'dist-webgl', 'dist-gpu']) {
  for (const file of ['jsui.js', 'jsui.wasm']) {
    const p = path.join(WEB, dir, file);
    if (!fs.existsSync(p) || fs.statSync(p).size === 0) missing.push(`web/${dir}/${file}`);
  }
}
if (missing.length) {
  console.error(`@orktes/jsui-web: missing WASM build outputs:\n  ${missing.join('\n  ')}\n` +
    'Build them with scripts/build-wasm.sh (plain, WEBGL=1 and GPU=1) or download the CI artifacts.');
  process.exit(1);
}
