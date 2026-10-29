#!/usr/bin/env node
// Summarizes run.sh's JSON files as a Markdown table (medians over runs).
//
//   node tools/jsui-impeller-spike/report.mjs <out-dir>
import fs from 'node:fs';
import path from 'node:path';

const dir = process.argv[2];
if (!dir) {
  console.error('usage: report.mjs <out-dir>');
  process.exit(2);
}
const median = (v) => {
  const s = [...v].sort((a, b) => a - b);
  return s.length ? s[Math.floor((s.length - 1) / 2)] : NaN;
};
const groups = new Map();
for (const f of fs.readdirSync(dir).filter((f) => f.endsWith('.json'))) {
  const name = f.replace(/-\d+\.json$/, '').replace(/\.json$/, '');
  const j = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8'));
  if (!groups.has(name)) groups.set(name, []);
  groups.get(name).push(j);
}
const cols = [
  ['context ms', (j) => j.context_ms],
  ['first present ms', (j) => j.first_present_ms],
  ['max frame ms', (j) => j.all.max],
  ['p99 ms', (j) => j.all.p99],
  ['p50 ms', (j) => j.all.p50],
  ['frames >33 ms', (j) => j.all.over33],
  ['stall ms', (j) => j.all.stall_ms],
  ['CPU record ms', (j) => j.all.record_ms],
  ['CPU submit ms', (j) => j.all.submit_ms],
  ['peak MB', (j) => j.mem_peak_mb],
  ['first 10 frames ms', (j) => first10(j)],
];
// The first 10 present-to-present intervals of every screen, summed: where a
// screen's first-use pipeline work lands. Single intervals are paced by the
// layer's drawable pool (~8-24 ms either way), so max / p99 hide it.
function first10(j) {
  let at = 0;
  let sum = 0;
  for (const s of j.screens) {
    sum += j.intervals.slice(at, at + 10).reduce((a, b) => a + b, 0);
    at += s.summary.frames;
  }
  return sum;
}
const order = ['graphite-bare', 'graphite-first', 'graphite-second', 'impeller-first', 'impeller-second',
  'graphite-1440p', 'impeller-1440p'];
const names = [...groups.keys()].sort((a, b) => (order.indexOf(a) + 1 || 99) - (order.indexOf(b) + 1 || 99));
const fmt = (x) => (Number.isFinite(x) ? (Math.abs(x) >= 100 ? x.toFixed(0) : x.toFixed(2)) : '-');
console.log(`| setup (runs) | ${cols.map((c) => c[0]).join(' | ')} |`);
console.log(`|---|${cols.map(() => '---:').join('|')}|`);
for (const n of names) {
  const js = groups.get(n);
  console.log(`| ${n} (${js.length}) | ${cols.map(([, get]) => fmt(median(js.map(get)))).join(' | ')} |`);
}
// Per-screen worst frame on first launches: where the cold hitches are.
const screens = new Set();
for (const n of names) for (const j of groups.get(n)) for (const s of j.screens) screens.add(path.basename(s.bundle));
console.log('\nWorst frame per screen (ms, median over runs)\n');
console.log(`| setup | ${[...screens].join(' | ')} |`);
console.log(`|---|${[...screens].map(() => '---:').join('|')}|`);
for (const n of names) {
  const js = groups.get(n);
  const row = [...screens].map((s) =>
    fmt(median(js.map((j) => j.screens.find((x) => path.basename(x.bundle) === s)?.summary.max ?? NaN))));
  console.log(`| ${n} | ${row.join(' | ')} |`);
}
const imp = groups.get('impeller-first')?.[0]?.unsupported;
if (imp && Object.keys(imp).length) {
  console.log('\nNot translated to Impeller (first impeller-first run, draw counts):');
  for (const [k, v] of Object.entries(imp)) console.log(`- ${k}: ${v}`);
}
