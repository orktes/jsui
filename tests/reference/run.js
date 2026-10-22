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
import { buildJsuiBundle, frameName, jsuiBuilderKind, jsuiRenderAvailable, renderJsuiFrames, runJsuiRender } from './lib/jsui.js';
import { motionChecks } from './lib/motion.js';
import { comparePngs, regionMismatchPct, worstTile } from './lib/compare.js';
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
      const scale = opts.scale * (c.meta.scale || 1);
      const r = { name: c.name, meta: c.meta, scale, chromeOnly: opts.chromeOnly, status: 'rendered', webLogs: [] };
      r.expectedPng = path.join(dir, 'expected.png');
      try {
        const webDir = await buildWebBundle(c.file, path.join(dir, 'web'));
        if (c.meta.frames) {
          // Animated case: every frame, uncached (cheap: one page).
          r.expectedFrames = [];
          await fs.promises.mkdir(path.join(dir, 'expected'), { recursive: true });
          const { logs } = await chrome.renderFrames(webDir, c.meta, scale, frameList(c.meta), async (i, png) => {
            const f = path.join(dir, 'expected', frameName(i));
            await fs.promises.writeFile(f, png);
            r.expectedFrames.push(f);
          });
          await fs.promises.copyFile(r.expectedFrames[keyFrame(c.meta)], r.expectedPng);
          r.webLogs = logs;
          results.push(r);
          continue;
        }
        const { png, cached, logs } = await chrome.render(webDir, c.meta, scale, { useCache: opts.cache });
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
        if (c.meta.frames) {
          r.actualFrames = await renderJsuiFrames(bundle, path.join(dir, 'actual'), c.meta, r.scale, frameList(c.meta), 2);
          await fs.promises.copyFile(r.actualFrames[keyFrame(c.meta)], r.actualPng);
          if (r.expectedFrames) compareAnimated(r, c.meta, dir);
          return;
        }
        const run = await runJsuiRender(bundle, r.actualPng, c.meta, r.scale);
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
      const local = localChecks(cmp, c.meta, r.scale);
      r.localFailures = local.filter((l) => !l.ok);
      r.status = cmp.mismatchPct <= c.meta.maxMismatch && !cmp.sizeMismatch && !r.localFailures.length ? 'pass' : 'fail';
      if (cmp.sizeMismatch) r.error = 'expected and actual sizes differ';
      if (r.localFailures.length) r.error = (r.error ? r.error + '\n' : '') + r.localFailures.map((l) => l.text).join('\n');
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
    if (r.notes) for (const t of r.notes) console.log(color(90, '     ' + t));
    else for (const l of r.localFailures || []) console.log(color(90, '     ' + l.text));
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

// Local checks that a whole-image percentage can hide:
//   meta.maxTileMismatch  worst share (%) of mismatched pixels in any
//                         meta.tileSize (default 16) CSS-px tile
//   meta.regions          [{ name, x, y, w, h, maxMismatch }] in CSS px
function localChecks(cmp, meta, scale) {
  const out = [];
  if (meta.maxTileMismatch != null) {
    const size = Math.round((meta.tileSize || 16) * scale);
    const t = worstTile(cmp, size);
    out.push({
      ok: t.pct <= meta.maxTileMismatch,
      text: `worst ${meta.tileSize || 16}px tile at (${Math.round(t.x / scale)}, ${Math.round(t.y / scale)}): ${t.pct.toFixed(1)}% (max ${meta.maxTileMismatch}%)`,
    });
  }
  for (const g of meta.regions || []) {
    const pct = regionMismatchPct(cmp, g.x * scale, g.y * scale, g.w * scale, g.h * scale);
    out.push({ ok: pct <= g.maxMismatch, text: `region "${g.name}": ${pct.toFixed(1)}% (max ${g.maxMismatch}%)` });
  }
  return out;
}

// Animated cases (examples/animated, meta.frames): frames 0..frames-1; the
// report shows the middle frame.
const frameList = (meta) => Array.from({ length: meta.frames }, (_, i) => i);
const keyFrame = (meta) => meta.frames >> 1;

// Every frame is pixel-compared (the worst frame sets mismatchPct, and the
// local checks run on it), then lib/motion.js checks that text moves
// rigidly with its box and boxes sit where Chrome puts them.
function compareAnimated(r, meta, dir) {
  const exp = r.expectedFrames.map((f) => fs.readFileSync(f));
  const act = r.actualFrames.map((f) => fs.readFileSync(f));
  let worst = null;
  let worstFrame = 0;
  exp.forEach((e, i) => {
    const cmp = comparePngs(e, act[i]);
    if (!worst || cmp.mismatchPct > worst.mismatchPct || cmp.sizeMismatch) {
      worst = cmp;
      worstFrame = i;
    }
  });
  r.diffPng = path.join(dir, 'diff.png');
  fs.writeFileSync(r.diffPng, worst.diffPng);
  fs.copyFileSync(r.expectedFrames[worstFrame], r.expectedPng);
  fs.copyFileSync(r.actualFrames[worstFrame], r.actualPng);
  r.mismatchPct = worst.mismatchPct;
  r.sizeMismatch = worst.sizeMismatch;
  const local = localChecks(worst, meta, r.scale);
  if (meta.motion) local.push(...motionChecks(exp, act, meta, r.scale));
  r.localFailures = local.filter((l) => !l.ok);
  r.motion = local.filter((l) => l.stats).map((l) => l.text);
  r.status = worst.mismatchPct <= meta.maxMismatch && !worst.sizeMismatch && !r.localFailures.length ? 'pass' : 'fail';
  const notes = [`worst frame ${worstFrame} of ${exp.length}`, ...r.motion];
  if (worst.sizeMismatch) notes.push('expected and actual sizes differ');
  for (const l of r.localFailures) if (!r.motion.includes(l.text)) notes.push(l.text);
  r.error = r.status === 'pass' ? undefined : notes.join('\n');
  r.notes = notes;
}

main().catch((e) => {
  console.error(e.stack || e);
  process.exit(1);
});
