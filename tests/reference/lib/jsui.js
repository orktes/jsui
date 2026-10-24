// JSUI side ("actual"): bundle the example for JSUI and run jsui-render.
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import * as esbuild from 'esbuild';
import { FONTS_DIR, INTER_FACES, JSUI_RENDER, PKG_DIR, REPO_ROOT } from './paths.js';

const require = createRequire(path.join(REPO_ROOT, 'package.json'));
const FALLBACK_RN = path.join(PKG_DIR, 'jsui-fallback/react-native.js');

function tryResolve(id) {
  try {
    return require.resolve(id);
  } catch {
    return null;
  }
}

/**
 * Which implementation backs `react-native` in JSUI bundles:
 *   'cli'      - @orktes/jsui-cli's programmatic build() (preferred, what users run)
 *   'package'  - esbuild + alias to @orktes/jsui-react
 *   'fallback' - esbuild + the minimal reconciler in jsui-fallback/ (only
 *                until @orktes/jsui-react exists; proves the pipeline end to end)
 * JSUI_REF_BUILDER=cli|package|fallback forces one.
 */
export async function jsuiBuilderKind() {
  const forced = process.env.JSUI_REF_BUILDER;
  if (forced) return forced;
  if (await loadCliBuild()) return 'cli';
  if (tryResolve('@orktes/jsui-react')) return 'package';
  return 'fallback';
}

let cliBuild;
async function loadCliBuild() {
  if (cliBuild !== undefined) return cliBuild;
  cliBuild = null;
  const resolved = tryResolve('@orktes/jsui-cli');
  if (resolved) {
    try {
      const mod = await import(resolved);
      if (typeof mod.build === 'function') cliBuild = mod.build;
    } catch {
      cliBuild = null;
    }
  }
  return cliBuild;
}

// The RN-standard bootstrap: register, then run (react-native-web and JSUI
// both implement AppRegistry).
function entrySource(appFile) {
  return `import { AppRegistry } from 'react-native';
import App from ${JSON.stringify(appFile)};
AppRegistry.registerComponent('App', () => App);
if (typeof AppRegistry.runApplication === 'function') AppRegistry.runApplication('App', {});
`;
}

/**
 * Bundles `entry` (a file exporting a default RN component) into `outFile`;
 * imported images are emitted next to it and referenced by relative URL.
 */
export async function buildJsuiBundle(entry, outFile) {
  const outDir = path.dirname(outFile);
  await fs.promises.rm(outDir, { recursive: true, force: true });
  await fs.promises.mkdir(outDir, { recursive: true });
  // Keep the bootstrap inside examples/ so its imports resolve like the app's.
  const bootstrap = path.join(path.dirname(entry), `.jsui-entry-${path.basename(entry, path.extname(entry))}.jsx`);
  await fs.promises.writeFile(bootstrap, entrySource(entry));
  try {
    const kind = await jsuiBuilderKind();
    if (kind === 'cli') {
      // JSUI_REF_REACT_COMPILER=0|1 overrides the CLI's default (React
      // Compiler on/off), e.g. to check that both render the same pixels.
      const rc = process.env.JSUI_REF_REACT_COMPILER;
      const reactCompiler = rc === undefined || rc === '' ? undefined : rc !== '0';
      await (await loadCliBuild())({ entry: bootstrap, outfile: outFile, minify: false, sourcemap: false, reactCompiler });
      return { kind };
    }
    const target = kind === 'package' ? '@orktes/jsui-react' : FALLBACK_RN;
    await esbuild.build({
      entryPoints: [bootstrap],
      bundle: true,
      outfile: outFile,
      assetNames: 'assets/[name]-[hash]',
      format: 'iife',
      target: 'es2022',
      jsx: 'automatic',
      alias: { 'react-native': target },
      nodePaths: [path.join(PKG_DIR, 'node_modules'), path.join(REPO_ROOT, 'node_modules')],
      define: { 'process.env.NODE_ENV': '"production"', __DEV__: 'false' },
      loader: { '.png': 'file', '.jpg': 'file', '.jpeg': 'file', '.js': 'jsx' },
      logLevel: 'silent',
    });
    return { kind };
  } finally {
    await fs.promises.rm(bootstrap, { force: true });
  }
}

export function jsuiRenderAvailable() {
  return fs.existsSync(JSUI_RENDER);
}

/**
 * Renders `frames` of an animated case (see examples/animated): each frame is
 * a fresh jsui-render run of `bundle` with `globalThis.__FRAME = i` prepended,
 * so frames are exact and independent. Writes <outDir>/NNNN.png; resolves
 * the file paths in frame order.
 */
export async function renderJsuiFrames(bundle, outDir, meta, scale, frames, jobs = 4) {
  await fs.promises.mkdir(outDir, { recursive: true });
  const src = await fs.promises.readFile(bundle, 'utf8');
  const files = frames.map((i) => path.join(outDir, frameName(i)));
  let next = 0;
  const worker = async () => {
    while (next < frames.length) {
      const k = next++;
      const fb = path.join(path.dirname(bundle), `frame-${frames[k]}.js`);
      await fs.promises.writeFile(fb, `globalThis.__FRAME = ${frames[k]};\n${src}`);
      const run = await runJsuiRender(fb, files[k], { ...meta, waitMs: 50 }, scale);
      await fs.promises.rm(fb, { force: true });
      if (!run.ok) throw new Error(`jsui-render failed on frame ${frames[k]}\n${run.stderr}`);
    }
  };
  await Promise.all(Array.from({ length: Math.min(jobs, frames.length) }, worker));
  return files;
}

export const frameName = (i) => String(i).padStart(4, '0') + '.png';

/** Runs jsui-render; resolves { ok, stderr, ms }. */
export function runJsuiRender(bundle, outPng, meta, scale) {
  const args = [
    bundle,
    '-o', outPng,
    '--width', String(meta.width),
    '--height', String(meta.height),
    '--scale', String(scale),
    '--wait-ms', String(meta.waitMs ?? 300),
  ];
  for (const f of INTER_FACES) args.push('--font', `Inter=${path.join(FONTS_DIR, f.file)}`);
  const started = performance.now();
  return new Promise((resolve) => {
    const child = spawn(JSUI_RENDER, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let stderr = '';
    child.stdout.on('data', (d) => (stderr += d));
    child.stderr.on('data', (d) => (stderr += d));
    const timer = setTimeout(() => child.kill('SIGKILL'), 60_000);
    child.on('error', (e) => {
      clearTimeout(timer);
      resolve({ ok: false, stderr: String(e), ms: performance.now() - started });
    });
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      const exists = fs.existsSync(outPng);
      resolve({
        ok: code === 0 && exists,
        hasImage: exists,
        stderr: stderr + (signal ? `\nkilled by ${signal}` : code ? `\nexit code ${code}` : ''),
        ms: performance.now() - started,
      });
    });
  });
}
