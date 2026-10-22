#!/usr/bin/env node
// Frame-by-frame renders of an animated fixture on both sides, for
// inspecting motion (text vs. box alignment, snapping) that a single static
// screenshot cannot show.
//
//   node frames.js <fixture.jsx> --out <dir> [--frames 0-71 | --frames 0,5,9]
//                  [--scale 1] [--chrome-only | --jsui-only] [--jobs 4]
//
// The fixture's App reads its frame index from globalThis.__FRAME at startup
// and exposes globalThis.__setFrame (see examples/animated/spring-text.jsx);
// `meta.frames` is the default frame count. Output: <dir>/chrome/NNNN.png and
// <dir>/jsui/NNNN.png. Each JSUI frame is a fresh jsui-render run of the same
// bundle with the frame index prepended, so frames are independent and exact.
import fs from 'node:fs';
import path from 'node:path';
import { readMeta } from './lib/discover.js';
import { buildWebBundle, ChromeRenderer } from './lib/web.js';
import { buildJsuiBundle, frameName, renderJsuiFrames } from './lib/jsui.js';

function parseFrames(spec, count) {
  if (!spec) return Array.from({ length: count }, (_, i) => i);
  const out = [];
  for (const part of spec.split(',')) {
    const [a, b] = part.split('-').map(Number);
    for (let i = a; i <= (Number.isFinite(b) ? b : a); i++) out.push(i);
  }
  return out;
}


async function main() {
  const argv = process.argv.slice(2);
  const opts = { scale: 1, jobs: 4, chrome: true, jsui: true };
  let fixture;
  let frameSpec;
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--out') opts.out = path.resolve(argv[++i]);
    else if (a === '--frames') frameSpec = argv[++i];
    else if (a === '--scale') opts.scale = Number(argv[++i]);
    else if (a === '--jobs') opts.jobs = Number(argv[++i]);
    else if (a === '--chrome-only') opts.jsui = false;
    else if (a === '--jsui-only') opts.chrome = false;
    else if (!a.startsWith('-')) fixture = path.resolve(a);
    else throw new Error(`unknown option ${a}`);
  }
  if (!fixture || !opts.out) {
    console.error('usage: node frames.js <fixture.jsx> --out <dir> [--frames 0-71] [--scale S] [--chrome-only|--jsui-only]');
    process.exit(2);
  }
  const meta = { ...readMeta(fixture), waitMs: 50 };
  const frames = parseFrames(frameSpec, meta.frames || 60);
  const scale = opts.scale * (meta.scale || 1);

  if (opts.chrome) {
    const dir = path.join(opts.out, 'chrome');
    await fs.promises.mkdir(dir, { recursive: true });
    const webDir = await buildWebBundle(fixture, path.join(opts.out, 'build/web'));
    const chrome = new ChromeRenderer();
    await chrome.start();
    try {
      const { logs } = await chrome.renderFrames(webDir, meta, scale, frames, (i, png) => fs.promises.writeFile(path.join(dir, frameName(i)), png));
      if (logs.length) console.warn(logs.join('\n'));
    } finally {
      await chrome.stop();
    }
    console.log(`chrome: ${frames.length} frames -> ${dir}`);
  }

  if (opts.jsui) {
    const dir = path.join(opts.out, 'jsui');
    const bundle = path.join(opts.out, 'build/jsui/bundle.js');
    await buildJsuiBundle(fixture, bundle);
    await renderJsuiFrames(bundle, dir, meta, scale, frames, opts.jobs);
    console.log(`jsui: ${frames.length} frames -> ${dir}`);
  }
}

main().catch((e) => {
  console.error(e.stack || e);
  process.exit(1);
});
