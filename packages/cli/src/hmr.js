// Hot module replacement for `jsui dev` (React Fast Refresh).
//
// esbuild has no HMR, so the dev server still runs its normal bundle build
// (for errors and the module graph in its metafile) and then assembles a
// second, module-per-function bundle from it:
//
//   vendor   every module outside the app (node_modules, @orktes/jsui-react,
//            assets), bundled once by esbuild and exposed by resolved path;
//            rebuilt only when that set or one of its files changes
//   app      each app source file, run through react-refresh's Babel plugin
//            and esbuild's transform (to CommonJS), wrapped in
//            __jsui_hmr.define(id, deps, factory)
//   runtime  hmr-runtime.js, the module registry and update logic
//
// update(from) answers GET /__jsui/hmr?from=<version>: the define()s of the
// app modules that changed since `from` plus __jsui_hmr.apply(), or a full
// reload when the vendor part changed or `from` is unknown.

import * as esbuild from 'esbuild';
import { transformAsync } from '@babel/core';
import { createHash } from 'node:crypto';
import { readFile, stat } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { esbuildOptions, jsuiReactDir } from './config.js';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const RUNTIME = readFileSync(path.join(here, 'hmr-runtime.js'), 'utf8');
const APP_EXT = /\.(jsx?|tsx?|mjs|cjs|json)$/i;
const HISTORY = 64;

let refreshPlugin = null;
function babelRefresh() {
  if (!refreshPlugin) refreshPlugin = require('react-refresh/babel');
  return refreshPlugin;
}

const sha1 = (s) => createHash('sha1').update(s).digest('hex');

function loaderFor(file) {
  const ext = path.extname(file).slice(1).toLowerCase();
  if (ext === 'json') return 'json';
  if (ext === 'ts' || ext === 'tsx') return ext;
  return 'jsx';  // .js/.jsx/.mjs/.cjs (config.js loads .js as jsx too)
}

/**
 * @param {object} o
 * @param {string} o.entry  absolute path of the entry module
 * @param {'development'|'production'} [o.mode]
 * @param {Map<string, Buffer>} o.assets
 * @param {(msg: string) => void} [o.log]
 */
export function createHmr(o) {
  const mode = o.mode || 'development';
  const cwd = process.cwd();
  const reactDir = jsuiReactDir() + path.sep;
  const cliDir = path.dirname(here) + path.sep;
  const compiled = new Map();  // key -> { hash, code }
  const history = new Map();  // version -> { vendor, entry, hashes: Map(key -> hash) }
  let vendor = { fingerprint: null, code: '' };
  let current = null;

  const isApp = (key) => {
    if (/^[a-z][\w-]*:/i.test(key) && !/^[a-z]:[\\/]/i.test(key)) return false;  // other esbuild namespaces
    const abs = path.resolve(cwd, key);
    return APP_EXT.test(abs) && !abs.includes(`${path.sep}node_modules${path.sep}`) &&
      !abs.startsWith(reactDir) && !abs.startsWith(cliDir);
  };

  const define = { 'process.env.NODE_ENV': JSON.stringify(mode), __DEV__: String(mode === 'development'), global: 'globalThis' };

  async function compile(key, deps) {
    const abs = path.resolve(cwd, key);
    const source = await readFile(abs, 'utf8');
    const depsJson = JSON.stringify(deps);
    const hash = sha1(source + '\0' + depsJson);
    const hit = compiled.get(key);
    if (hit && hit.hash === hash) return hit;
    const loader = loaderFor(abs);
    let code = source;
    if (loader !== 'json') {
      const plugins = loader === 'ts' ? ['typescript'] : loader === 'tsx' ? ['typescript', 'jsx'] : ['jsx'];
      try {
        const r = await transformAsync(source, {
          filename: abs,
          babelrc: false,
          configFile: false,
          sourceType: 'unambiguous',
          parserOpts: { plugins },
          plugins: [[babelRefresh(), { skipEnvCheck: true }]],
          compact: false,
        });
        code = r.code;
      } catch (e) {
        // Syntax Babel does not know (esbuild already accepted the file):
        // the module still hot-updates through its exports, only hook
        // signatures are missing.
        o.log?.(`[jsui] hmr: React Refresh transform skipped for ${key}: ${e.message.split('\n')[0]}`);
      }
    }
    const out = await esbuild.transform(code, {
      loader,
      format: 'cjs',
      target: 'es2020',
      charset: 'utf8',
      jsx: 'automatic',
      jsxImportSource: 'react',
      jsxDev: false,
      define,
      sourcefile: key,
      logLevel: 'silent',
    });
    const entry = {
      hash,
      code: `__jsui_hmr.define(${JSON.stringify(key)}, ${depsJson}, function (require, module, exports, $RefreshReg$, $RefreshSig$) {\n${out.code}});\n`,
    };
    compiled.set(key, entry);
    return entry;
  }

  async function buildVendor(keys, fingerprint) {
    if (fingerprint === vendor.fingerprint) return false;
    const lines = [
      `var Refresh = require(${JSON.stringify(require.resolve('react-refresh/runtime'))});`,
      'Refresh.injectIntoGlobalHook(globalThis);',
      'globalThis.__jsui_hmr_refresh = Refresh;',
      'globalThis.__jsui_hmr_vendor = {',
      ...keys.map((k) => `  ${JSON.stringify(k)}: function () { return require(${JSON.stringify(path.resolve(cwd, k))}); },`),
      '};',
    ];
    const opts = esbuildOptions({ entry: 'x', outfile: path.join(cwd, '.jsui-dev', 'vendor.js'), mode, assets: o.assets });
    delete opts.entryPoints;
    delete opts.banner;
    opts.stdin = { contents: lines.join('\n'), resolveDir: cwd, sourcefile: 'jsui-hmr-vendor.js', loader: 'js' };
    opts.metafile = false;
    opts.sourcemap = false;
    const r = await esbuild.build(opts);
    vendor = { fingerprint, code: r.outputFiles[0].text };
    return true;
  }

  /**
   * Assembles the HMR bundle of a successful build. Returns the bundle text;
   * throws on errors (compile errors the bundle build did not catch).
   */
  async function commit(metafile, version) {
    const inputs = metafile.inputs;
    const keyOf = new Map(Object.keys(inputs).map((k) => [path.resolve(cwd, k), k]));
    const entryKey = keyOf.get(path.resolve(o.entry));
    if (!entryKey || !isApp(entryKey)) throw new Error(`hmr: ${o.entry} is not an app module of the build`);
    const app = Object.keys(inputs).filter(isApp);
    const vendorKeys = new Set();
    const graph = new Map();
    for (const key of app) {
      const deps = {};
      for (const r of inputs[key].imports) {
        if (r.external || r.original == null) continue;
        const target = keyOf.get(path.resolve(cwd, r.path)) || r.path;
        deps[r.original] = target;
        if (!isApp(target)) vendorKeys.add(target);
      }
      graph.set(key, deps);
    }
    // Any change outside the app (an edited package, a new dependency)
    // rebuilds the vendor part, and clients reload instead of hot-updating.
    const others = Object.keys(inputs).filter((k) => !isApp(k)).sort();
    const stats = await Promise.all(others.map(async (k) => {
      try {
        const s = await stat(path.resolve(cwd, k));
        return `${k}:${s.size}:${s.mtimeMs}`;
      } catch {
        return k;
      }
    }));
    const sortedVendor = [...vendorKeys].sort();
    const fingerprint = sha1(sortedVendor.join('\n') + '\0' + stats.join('\n'));
    await buildVendor(sortedVendor, fingerprint);
    const hashes = new Map();
    const code = new Map();
    await Promise.all(app.map(async (key) => {
      const c = await compile(key, graph.get(key));
      hashes.set(key, c.hash);
      code.set(key, c.code);
    }));
    for (const k of compiled.keys()) if (!hashes.has(k)) compiled.delete(k);
    current = { version, vendor: fingerprint, entry: entryKey, hashes, code };
    history.set(version, { vendor: fingerprint, entry: entryKey, hashes });
    while (history.size > HISTORY) history.delete(history.keys().next().value);
    const banner = esbuildOptions({ entry: 'x', outfile: 'x', mode, assets: o.assets }).banner.js;
    return [
      banner,
      vendor.code,
      RUNTIME,
      ...app.map((k) => code.get(k)),
      `__jsui_hmr.start(${JSON.stringify(entryKey)}, ${JSON.stringify(version)});\n`,
    ].join('\n');
  }

  /** The update script for a client running `from` (see hmr-runtime.js). */
  function update(from, error) {
    if (error) return `__jsui_hmr.buildError(${JSON.stringify(error)});\n`;
    if (!current) return `__jsui_hmr.reload("no successful build yet");\n`;
    const old = history.get(from);
    if (!old) return `__jsui_hmr.reload(${JSON.stringify(`the dev server does not know version ${from}`)});\n`;
    if (old.vendor !== current.vendor) return `__jsui_hmr.reload("dependencies or assets changed");\n`;
    if (old.entry !== current.entry) return `__jsui_hmr.reload("the entry module changed");\n`;
    const changed = [...current.hashes.keys()].filter((k) => old.hashes.get(k) !== current.hashes.get(k));
    return changed.map((k) => current.code.get(k)).join('') +
      `__jsui_hmr.apply(${JSON.stringify(changed)}, ${JSON.stringify(current.version)});\n`;
  }

  return { commit, update, get current() { return current; } };
}
