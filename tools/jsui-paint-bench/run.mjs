#!/usr/bin/env node
// Builds the paint-bench scenarios and runs jsui-paint-bench on them.
//
//   node tools/jsui-paint-bench/run.mjs [--bin build/jsui-paint-bench] [--out build/paint-bench]
//        [--only cards,text] [any jsui-paint-bench flag: --frames 240 --backends cpu,gl --json r.json ...]
//
// Scenarios: interactions (examples/showcase/interactions.jsx, idle: its
// looping animations), interactions-hover (same, with a mouse sweep), caret
// (static screen, blinking caret), cards (shadowed, rounded-clipped cards
// scrolling) and text (text-heavy panels with a ticking clock, a ticker and
// a pulsing badge).
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { build } from '@orktes/jsui-cli';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const args = process.argv.slice(2);
const take = (name, def) => {
  const i = args.indexOf(name);
  if (i < 0) return def;
  const v = args[i + 1];
  args.splice(i, 2);
  return v;
};
const exe = process.platform === 'win32' ? '.exe' : '';
const bin = path.resolve(ROOT, take('--bin', `build/jsui-paint-bench${exe}`));
const out = path.resolve(ROOT, take('--out', 'build/paint-bench'));
const only = take('--only', '');

const SCENES = [
  ['interactions', 'examples/showcase/interactions.jsx'],
  ['interactions-hover', 'examples/showcase/interactions.jsx', '@hover'],
  ['caret', 'tools/jsui-paint-bench/scenes/caret.jsx'],
  ['cards', 'tools/jsui-paint-bench/scenes/cards.jsx'],
  ['text', 'tools/jsui-paint-bench/scenes/text.jsx'],
].filter(([name]) => !only || only.split(',').includes(name));

const specs = [];
const built = new Map();
for (const [name, entry, suffix = ''] of SCENES) {
  let bundle = built.get(entry);
  if (!bundle) {
    bundle = path.join(out, path.basename(entry, path.extname(entry)), 'bundle.js');
    fs.mkdirSync(path.dirname(bundle), { recursive: true });
    await build({ entry: path.join(ROOT, entry), outfile: bundle, minify: true });
    built.set(entry, bundle);
  }
  specs.push(`${name}=${bundle}${suffix}`);
}
const r = spawnSync(bin, [...args, ...specs], { stdio: 'inherit' });
process.exit(r.status ?? 1);
