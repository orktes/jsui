// Hot module replacement for `jsui dev` (React Fast Refresh).
//
// esbuild has no HMR, so the dev server still runs its normal bundle build
// (for errors and the module graph in its metafile) and then assembles a
// second, module-per-function bundle from it:
//
//   vendor   every module outside the app (node_modules, @orktes/jsui-react,
//            assets), bundled once by esbuild and exposed by resolved path;
//            rebuilt only when that set or one of its files changes
//   app      each app source file, run through React Compiler (unless off,
//            see react-compiler.js), react-refresh's Babel plugin and
//            esbuild's transform (to CommonJS), wrapped in
//            __jsui_hmr.define(id, deps, factory)
//   runtime  hmr-runtime.js, the module registry and update logic
//
// update(from) answers GET /__jsui/hmr?from=<version>: the define()s of the
// app modules that changed since `from` plus __jsui_hmr.apply(), or a full
// reload when the vendor part changed or `from` is unknown.
//
// Source maps (unless `sourcemap: false`): each app module's map is the
// composition of its transforms (React Compiler's map is Babel's input map,
// Babel's output map rides into esbuild.transform as an inline map, which
// esbuild applies), the vendor part has esbuild's, the runtime part none.
// sourceMap() flattens them into the map of the bundle (built on request,
// once per version: rebuilds only pay for the per-module maps); hot update
// scripts carry an inline map of their define()s. Sources are file:// URLs
// with sourcesContent (sourcemap.js).

import * as esbuild from 'esbuild';
import { transformAsync } from '@babel/core';
import { createHash } from 'node:crypto';
import { readFile, stat } from 'node:fs/promises';
import { readFileSync, realpathSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { esbuildOptions, jsuiReactDir } from './config.js';
import { compileSource } from './react-compiler.js';
import { transformDerive } from './derive.js';
import { concatMaps, inlineMapComment, lineCount, prepareMap } from './sourcemap.js';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const RUNTIME = readFileSync(path.join(here, 'hmr-runtime.js'), 'utf8');
const APP_EXT = /\.(jsx?|tsx?|mjs|cjs|json)$/i;
const HISTORY = 64;

// esbuild realpaths its inputs (macOS: /var -> /private/var), so paths are
// compared after resolving symlinks.
function real(p) {
  try {
    return realpathSync.native(p);
  } catch {
    return p;
  }
}

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
 * @param {boolean} [o.reactCompiler]  compile app modules with React Compiler
 *   (the bundle build of dev.js runs it too, with compilerHmr: its metafile
 *   lists the modules' imports, react/compiler-runtime among them, and both
 *   share one compile per file version)
 * @param {boolean} [o.sourcemap]  source maps for the bundle and hot updates (default true)
 * @param {(msg: string) => void} [o.log]
 */
export function createHmr(o) {
  const mode = o.mode || 'development';
  const maps = o.sourcemap !== false;
  const cwd = process.cwd();
  const reactDir = real(jsuiReactDir()) + path.sep;
  const cliDir = real(path.dirname(here)) + path.sep;
  const compiled = new Map();  // key -> { hash, code, map }
  const history = new Map();  // version -> { vendor, entry, hashes: Map(key -> hash) }
  let vendor = { fingerprint: null, code: '', map: null };
  let mapCache = { version: null, text: null };
  let current = null;

  const isApp = (key) => {
    if (/^[a-z][\w-]*:/i.test(key) && !/^[a-z]:[\\/]/i.test(key)) return false;  // other esbuild namespaces
    const abs = real(path.resolve(cwd, key));
    return APP_EXT.test(abs) && !abs.includes(`${path.sep}node_modules${path.sep}`) &&
      !abs.startsWith(reactDir) && !abs.startsWith(cliDir);
  };

  const define = { 'process.env.NODE_ENV': JSON.stringify(mode), __DEV__: String(mode === 'development'), global: 'globalThis' };

  async function compile(key, deps) {
    const abs = path.resolve(cwd, key);
    const source = (await transformDerive(await readFile(abs, 'utf8'), abs)).code;
    const depsJson = JSON.stringify(deps);
    const hash = sha1(source + '\0' + depsJson);
    const hit = compiled.get(key);
    if (hit && hit.hash === hash) return hit;
    const loader = loaderFor(abs);
    let code = source;
    let map = null;  // of `code` to `source`
    if (loader !== 'json' && o.reactCompiler) {
      // First, as React Compiler expects to see the source as written. It
      // adds a source hash check that resets the components' memo caches
      // when the file is hot-updated.
      try {
        const r = await compileSource(source, abs, { hmr: true, map: maps });
        code = r.code;
        map = r.map || null;
      } catch (e) {
        o.log?.(`[jsui] hmr: React Compiler skipped ${key}: ${e.message.split('\n')[0]}`);
      }
    }
    if (loader !== 'json') {
      const plugins = loader === 'ts' ? ['typescript'] : loader === 'tsx' ? ['typescript', 'jsx'] : ['jsx'];
      try {
        const r = await transformAsync(code, {
          filename: abs,
          babelrc: false,
          configFile: false,
          sourceType: 'unambiguous',
          parserOpts: { plugins },
          plugins: [[babelRefresh(), { skipEnvCheck: true }]],
          compact: false,
          sourceMaps: maps,
          sourceFileName: abs,
          inputSourceMap: map || false,
        });
        code = r.code;
        if (maps) map = r.map || null;
      } catch (e) {
        // Syntax Babel does not know (esbuild already accepted the file):
        // the module still hot-updates through its exports, only hook
        // signatures are missing.
        o.log?.(`[jsui] hmr: React Refresh transform skipped for ${key}: ${e.message.split('\n')[0]}`);
      }
    }
    // esbuild.transform applies an inline input map (checked: the output
    // map's sources and sourcesContent are the original file's).
    if (map) code += '\n' + inlineMapComment(map) + '\n';
    const out = await esbuild.transform(code, {
      loader,
      sourcemap: maps ? 'external' : false,
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
      // The module's code starts on the line after define(.
      code: `__jsui_hmr.define(${JSON.stringify(key)}, ${depsJson}, function (require, module, exports, $RefreshReg$, $RefreshSig$) {\n${out.code}});\n`,
      map: maps && out.map ? prepareMap(out.map, cwd) : null,
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
    opts.sourcemap = maps ? 'external' : false;
    const r = await esbuild.build(opts);
    const js = r.outputFiles.find((f) => f.path.endsWith('.js'));
    const map = r.outputFiles.find((f) => f.path.endsWith('.map'));
    vendor = { fingerprint, code: js.text, map: map ? prepareMap(map.text, path.dirname(opts.outfile)) : null };
    return true;
  }

  /**
   * Assembles the HMR bundle of a successful build. Returns the bundle text;
   * throws on errors (compile errors the bundle build did not catch).
   */
  async function commit(metafile, version) {
    const inputs = metafile.inputs;
    const keyOf = new Map(Object.keys(inputs).map((k) => [real(path.resolve(cwd, k)), k]));
    const entryKey = keyOf.get(real(path.resolve(o.entry)));
    if (!entryKey || !isApp(entryKey)) throw new Error(`hmr: ${o.entry} is not an app module of the build`);
    const app = Object.keys(inputs).filter(isApp);
    const vendorKeys = new Set();
    const graph = new Map();
    for (const key of app) {
      const deps = {};
      for (const r of inputs[key].imports) {
        if (r.external || r.original == null) continue;
        const target = keyOf.get(real(path.resolve(cwd, r.path))) || r.path;
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
    const banner = esbuildOptions({ entry: 'x', outfile: 'x', mode, assets: o.assets }).banner.js;
    const chunks = [
      [banner, null],
      [vendor.code, vendor.map],
      [RUNTIME, null],
      ...app.map((k) => [code.get(k), compiled.get(k).map, 1]),
      [`__jsui_hmr.start(${JSON.stringify(entryKey)}, ${JSON.stringify(version)});\n`, null],
    ];
    // Where each part of the map starts (the chunks are joined with '\n').
    const parts = [];
    let line = 0;
    for (const [text, map, skip = 0] of chunks) {
      if (maps && map) parts.push({ line: line + skip, map });
      line += lineCount(text);
    }
    current = { version, vendor: fingerprint, entry: entryKey, hashes, code, maps: new Map(app.map((k) => [k, compiled.get(k).map])), parts };
    history.set(version, { vendor: fingerprint, entry: entryKey, hashes });
    while (history.size > HISTORY) history.delete(history.keys().next().value);
    const text = chunks.map((c) => c[0]).join('\n');
    return maps ? text + '//# sourceMappingURL=bundle.js.map\n' : text;
  }

  /** The source map (JSON text) of the last committed bundle, or null. */
  function sourceMap() {
    if (!maps || !current) return null;
    if (mapCache.version !== current.version) mapCache = { version: current.version, text: JSON.stringify(concatMaps(current.parts, 'bundle.js')) };
    return mapCache.text;
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
    const script = changed.map((k) => current.code.get(k)).join('') +
      `__jsui_hmr.apply(${JSON.stringify(changed)}, ${JSON.stringify(current.version)});\n`;
    if (!maps || !changed.length) return script;
    const parts = [];
    let line = 0;
    for (const k of changed) {
      const map = current.maps.get(k);
      if (map) parts.push({ line: line + 1, map });
      line += lineCount(current.code.get(k)) - 1;  // each ends with '\n'
    }
    return script + inlineMapComment(concatMaps(parts, `${current.version}.js`)) + '\n';
  }

  return { commit, update, sourceMap, get current() { return current; } };
}
