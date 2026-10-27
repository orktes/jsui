#!/usr/bin/env node
// Records the GPU pipelines the showcase draws with into
// site/public/pipelines-{bgra8unorm,rgba8unorm}.bin, which index.html hands
// to createJsui({ pipelines }) so a first visit builds them under the splash
// (see "GPU pipelines" in web/README.md). Run it on a machine with a GPU
// after building the site, and again when Skia is bumped (a list of another
// Skia build is skipped) or new screens add kinds of draws:
//
//   node site/scripts/build.mjs && node site/scripts/record-pipelines.mjs [--dir site/dist]
//
// Plays the intro, then visits every route and screen (the smoke test's
// list) with real Chrome through Playwright. The old lists are left out of
// the recording run, so the new ones hold only what the site uses now.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { recordPipelines } from '@orktes/jsui-cli';

const SITE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const di = args.indexOf('--dir');
const DIR = path.resolve(di >= 0 ? args[di + 1] : path.join(SITE, 'dist'));
const FORMATS = ['bgra8unorm', 'rgba8unorm'];

const ROUTES = ['/', '/examples', '/examples/main-menu', '/examples/hud', '/examples/inventory', '/examples/battle-pass',
  '/examples/scoreboard', '/examples/dialogue', '/examples/settings', '/examples/loading', '/examples/notifications',
  '/examples/cinematic', '/examples/victory', '/examples/rift', '/use-cases', '/features', '/features/interactions', '/features/input', '/features/motion', '/features/vector', '/features/native', '/playground', '/embed'];

for (const f of FORMATS) fs.rmSync(path.join(DIR, `pipelines-${f}.bin`), { force: true });
const out = await recordPipelines({
  dir: DIR,
  // The intro first (?intro=1 plays it whatever the renderer guess), then
  // each route in the running app; ?autoplay=0: no trailer sound.
  visits: ['?intro=1&autoplay=0', ...ROUTES.map((r) => '#' + r)],
  waitMs: 4000,  // the intro takes 3.4 s
  formats: FORMATS,
  log: (t) => console.log(t),
});
for (const [format, r] of Object.entries(out)) {
  fs.copyFileSync(r.file, path.join(SITE, 'public', `pipelines-${format}.bin`));
  console.log(`site/public/pipelines-${format}.bin: ${r.known} pipelines, ${r.bytes} bytes`);
}
