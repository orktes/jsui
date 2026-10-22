#!/usr/bin/env node
// Reference tests: render every example with react-native-web in headless
// Chromium (expected) and with jsui-render (actual), then compare pixels.
//
//   node run.js [filters...] [--chrome-only] [--scale 2] [--no-cache] [--jobs N]
//
// See README.md.
import fs from 'node:fs';
import path from 'node:path';
import { discoverCases } from './lib/discover.js';
import { buildWebBundle, ChromeRenderer, rnwVersion } from './lib/web.js';
import { buildJsuiBundle, jsuiBuilderKind, jsuiRenderAvailable, runJsuiRender } from './lib/jsui.js';
import { comparePngs } from './lib/compare.js';
import { writeReport } from './lib/report.js';
import { JSUI_RENDER, OUTPUT_DIR } from './lib/paths.js';

function parseArgs(argv) {
  const opts = { filters: [], chromeOnly: false, scale: 1, cache: true, jobs: 2 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => (a.includes('=') ? a.split('=')[1] : argv[++i]);
    if (a === '--chrome-only' || a === '--web-only') opts.chromeOnly = true;
    else if (a === '--no-cache') opts.cache = false;
    else if (a.startsWith('--scale')) opts.scale = Number(val());
    else if (a.startsWith('--jobs')) opts.jobs = Math.max(1, Number(val()));
    else if (a === '-h' || a === '--help') {
      console.log('usage: node run.js [filters...] [--chrome-only] [--scale 1|2] [--no-cache] [--jobs N]');
      process.exit(0);
    } else if (a.startsWith('-')) {
      console.error(`unknown option ${a}`);
      process.exit(2);
    } else opts.filters.push(a);
  }
  if (!(opts.scale > 0)) throw new Error('--scale must be > 0');
  return opts;
}

// Small promise pool so jsui-render processes run in parallel without
// oversubscribing a shared machine.
async function pool(items, n, fn) {
  const out = new Array(items.length);
  let next = 0;
  await Promise.all(
    Array.from({ length: Math.min(n, items.length) }, async () => {
      while (next < items.length) {
        const i = next++;
        out[i] = await fn(items[i], i);
      }
    }),
  );
  return out;
}

const color = (code, s) => (process.stdout.isTTY ? `\x1b[${code}m${s}\x1b[0m` : s);

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const cases = await discoverCases(opts.filters);
  if (!cases.length) {
    console.error('no example cases matched');
    process.exit(2);
  }
  const suffix = opts.scale === 1 ? '' : `@${opts.scale}x`;
  const caseDir = (c) => path.join(OUTPUT_DIR, 'cases', c.name.replace('/', '__') + suffix);

  let jsuiNote = '';
  if (!opts.chromeOnly) {
    if (!jsuiRenderAvailable()) {
      console.error(`jsui-render not found at ${JSUI_RENDER}. Build it (cmake --build build) or set JSUI_RENDER, or pass --chrome-only.`);
      process.exit(2);
    }
    const kind = await jsuiBuilderKind();
    jsuiNote = `JSUI bundles via ${kind}`;
    if (kind === 'fallback') console.warn(color(33, 'note: @jsui/react not found; JSUI bundles use tests/reference/jsui-fallback'));
  }

  // 1. Chrome (expected) — one browser, sequential pages.
  const chrome = new ChromeRenderer();
  await chrome.start();
  const results = [];
  try {
    for (const c of cases) {
      const dir = caseDir(c);
      await fs.promises.mkdir(dir, { recursive: true });
      const r = { name: c.name, meta: c.meta, scale: opts.scale, chromeOnly: opts.chromeOnly, status: 'rendered', webLogs: [] };
      r.expectedPng = path.join(dir, 'expected.png');
      try {
        const webDir = await buildWebBundle(c.file, path.join(dir, 'web'));
        const { png, cached, logs } = await chrome.render(webDir, c.meta, opts.scale, { useCache: opts.cache });
        await fs.promises.writeFile(r.expectedPng, png);
        r.webLogs = logs;
        r.cached = cached;
      } catch (e) {
        r.status = 'error';
        r.error = `Chrome side failed: ${e.stack || e}`;
        r.expectedPng = null;
      }
      results.push(r);
    }
  } finally {
    await chrome.stop();
  }

  // 2. JSUI (actual) + compare.
  if (!opts.chromeOnly) {
    await pool(results, opts.jobs, async (r, i) => {
      const c = cases[i];
      const dir = caseDir(c);
      r.actualPng = path.join(dir, 'actual.png');
      await fs.promises.rm(r.actualPng, { force: true });
      try {
        const bundle = path.join(dir, 'jsui', 'bundle.js');
        await buildJsuiBundle(c.file, bundle);
        const run = await runJsuiRender(bundle, r.actualPng, c.meta, opts.scale);
        r.jsuiMs = run.ms;
        r.jsuiLog = run.stderr.trim();
        if (!run.hasImage) throw new Error(`jsui-render produced no image\n${run.stderr}`);
        if (!run.ok && r.status !== 'error') r.renderWarning = true;
      } catch (e) {
        r.status = 'error';
        r.error = (r.error ? r.error + '\n' : '') + `JSUI side failed: ${e.message || e}`;
        r.actualPng = null;
        return;
      }
      if (!r.expectedPng) return;
      const cmp = comparePngs(fs.readFileSync(r.expectedPng), fs.readFileSync(r.actualPng));
      r.diffPng = path.join(dir, 'diff.png');
      fs.writeFileSync(r.diffPng, cmp.diffPng);
      r.mismatchPct = cmp.mismatchPct;
      r.sizeMismatch = cmp.sizeMismatch;
      r.status = cmp.mismatchPct <= c.meta.maxMismatch && !cmp.sizeMismatch ? 'pass' : 'fail';
      if (cmp.sizeMismatch) r.error = 'expected and actual sizes differ';
      if (r.renderWarning) r.error = (r.error ? r.error + '\n' : '') + 'jsui-render reported an error (see logs)';
    });
  }

  // 3. Report.
  const info = [
    `react-native-web ${rnwVersion()}`,
    `Chromium ${chrome.version}`,
    jsuiNote,
    `scale ${opts.scale}`,
    new Date().toISOString().replace('T', ' ').slice(0, 19),
  ].filter(Boolean).join(' · ');
  const reportFile = path.join(OUTPUT_DIR, `report${suffix}.html`);
  writeReport(reportFile, { results, info });
  const summary = results.map(({ name, status, mismatchPct, meta, error, cached, jsuiMs }) => ({
    name, status, mismatchPct, maxMismatch: meta.maxMismatch, width: meta.width, height: meta.height, error, chromeCached: cached, jsuiMs,
  }));
  fs.writeFileSync(path.join(OUTPUT_DIR, `results${suffix}.json`), JSON.stringify({ info, results: summary }, null, 2));

  const w = Math.max(...results.map((r) => r.name.length));
  for (const r of results) {
    const pct = r.mismatchPct == null ? '' : `${r.mismatchPct.toFixed(2).padStart(6)}% (max ${r.meta.maxMismatch}%)`;
    const tag = { pass: color(32, 'PASS'), fail: color(31, 'FAIL'), error: color(33, 'ERR '), rendered: color(36, 'OK  ') }[r.status];
    console.log(`${tag} ${r.name.padEnd(w)} ${pct}${r.cached ? color(90, ' [chrome cached]') : ''}`);
    if (r.status === 'error' && r.error) console.log(color(90, '     ' + r.error.split('\n')[0]));
  }
  console.log(`\nreport: ${path.relative(process.cwd(), reportFile) || reportFile}`);
  const bad = results.filter((r) => r.status === 'fail' || r.status === 'error').length;
  if (bad) {
    console.log(color(31, `${bad} of ${results.length} cases failed`));
    process.exitCode = 1;
  } else {
    console.log(color(32, opts.chromeOnly ? `${results.length} cases rendered in Chrome` : `all ${results.length} cases passed`));
  }
}

main().catch((e) => {
  console.error(e.stack || e);
  process.exit(1);
});
